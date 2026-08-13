#include "../lpr_filed_internal.h"

void *lpr_termd_payload(void *page)
{
    return page == 0 ? 0 : (void *)((uint8_t *)page + PACHA_SERVICE_HEADER_BYTES);
}

static int64_t lpr_termd_call_with_fd(
    uint32_t op,
    int page_fd,
    void *page,
    uint32_t payload_size,
    int transfer_fd,
    uint64_t *out_result)
{
    if (LPR_TERMD_TTY_ENDPOINT_FD < 16) {
        return -LPR_LINUX_ENOTTY;
    }
    if (page_fd < 16 || page == 0 ||
        payload_size > TERMD_PAGE_BYTES - PACHA_SERVICE_HEADER_BYTES)
    {
        return -LPR_LINUX_EINVAL;
    }
    lpr_signal_thread_state_t *thread_state = lpr_signal_thread_state_current();
    struct pacha_ipc_fd *fds = thread_state->termd_fds;
    struct pacha_ipc_msg *request = &thread_state->termd_request;
    struct pacha_ipc_msg *reply = &thread_state->termd_reply;
    lpr_zero_bytes(fds, sizeof(thread_state->termd_fds));
    lpr_zero_bytes(request, sizeof(*request));
    lpr_zero_bytes(reply, sizeof(*reply));

    const uint64_t request_id = lpr_next_request_id(&lpr_termd_request_id);
    pacha_service_envelope_t *header = (pacha_service_envelope_t *)page;
    lpr_memset(header, 0, sizeof(*header));
    header->magic = PACHA_SERVICE_REQUEST_MAGIC;
    header->abi_version = PACHA_SERVICE_ABI_VERSION;
    header->service_id = TERMD_SERVICE_ID;
    header->op = op;
    header->flags = payload_size != 0 ? PACHA_SERVICE_FLAG_PAGE_PAYLOAD : 0;
    header->request_id = request_id;
    header->trace_id = request_id;
    header->payload_size = payload_size;

    uint64_t fd_count = 0;
    fds[fd_count].fd = (uint64_t)(uint32_t)page_fd;
    fds[fd_count].rights =
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    fd_count++;
    if (transfer_fd >= 16) {
        fds[fd_count].fd = (uint64_t)(uint32_t)transfer_fd;
        fds[fd_count].rights =
            PACHA_FD_RIGHT_INSPECT |
            PACHA_FD_RIGHT_TRANSFER |
            PACHA_FD_RIGHT_DUP |
            PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_SEND |
            PACHA_FD_RIGHT_RECV |
            PACHA_FD_RIGHT_WAIT |
            PACHA_FD_RIGHT_POLL;
        fds[fd_count].transfer_flags = PACHA_IPC_TRANSFER_MOVE;
        fd_count++;
    }

    request->word0 = PACHA_SERVICE_REQUEST_MAGIC;
    request->word1 = 0;
    request->word2 = 0;
    request->word3 = request_id;
    request->fds = fds;
    request->fd_count = fd_count;
    const int64_t reply_fd = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_IPC_CALL,
        LPR_TERMD_TTY_ENDPOINT_FD,
        (uint64_t)(uintptr_t)request);
    if (reply_fd < 16) {
        const int64_t err = lpr_pacha_status_to_errno(reply_fd);
        lpr_trace_error_record(
            LPR_ERROR_DOMAIN_KERNEL,
            op,
            LPR_ERROR_STAGE_CHILD_RPC_CALL,
            err,
            reply_fd,
            request->word3,
            fd_count,
            LPR_TERMD_TTY_ENDPOINT_FD,
            0,
            "termd ipc_call failed");
        return lpr_pacha_status_to_errno(reply_fd);
    }
    const int64_t recv_status = lpr_native_ipc_recv_wait(
        (uint64_t)(uint32_t)reply_fd,
        reply);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)reply_fd);
    if (recv_status != 0) {
        const int64_t err = lpr_pacha_status_to_errno(recv_status);
        lpr_trace_error_record(
            LPR_ERROR_DOMAIN_KERNEL,
            op,
            LPR_ERROR_STAGE_CHILD_RPC_RECV,
            err,
            recv_status,
            request->word3,
            fd_count,
            (uint64_t)(uint32_t)reply_fd,
            0,
            "termd reply recv failed");
        return err;
    }
    const pacha_service_envelope_t *reply_header =
        (const pacha_service_envelope_t *)page;
    if (reply->word0 != PACHA_SERVICE_REPLY_MAGIC ||
        reply->word3 != request->word3 ||
        reply_header->magic != PACHA_SERVICE_REPLY_MAGIC ||
        reply_header->request_id != request->word3 ||
        reply_header->service_id != TERMD_SERVICE_ID ||
        reply_header->op != op)
    {
        lpr_trace_error_record(
            LPR_ERROR_DOMAIN_TERMD,
            op,
            LPR_ERROR_STAGE_REPLY_MAGIC,
            -LPR_LINUX_EIO,
            (int64_t)reply->word0,
            request->word3,
            fd_count,
            reply->word3,
            reply->word2,
            "termd reply mismatch");
        return -LPR_LINUX_EIO;
    }
    if ((int64_t)reply->word1 < 0) {
        lpr_trace_error_record(
            LPR_ERROR_DOMAIN_TERMD,
            op,
            LPR_ERROR_STAGE_CHILD_STATUS,
            (int64_t)reply->word1,
            (int64_t)reply->word1,
            request->word3,
            fd_count,
            0,
            reply->word2,
            "termd returned error");
    }
    if (out_result != 0) {
        *out_result = reply_header->result;
    }
    return reply_header->status;
}

int64_t lpr_termd_call(
    uint32_t op,
    int page_fd,
    void *page,
    uint32_t payload_size,
    uint64_t *out_result)
{
    return lpr_termd_call_with_fd(
        op, page_fd, page, payload_size, -1, out_result);
}

int64_t lpr_termd_call_handle(uint32_t op, uint64_t handle, uint64_t *out_result)
{
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    termd_handle_request_t *req = (termd_handle_request_t *)lpr_termd_payload(page);
    lpr_memset(req, 0, sizeof(*req));
    req->handle = handle;
    lpr_fill_termd_caller(&req->tty.session_id, &req->tty.process_id, &req->tty.pgrp_id);
    lpr_fill_termd_signal_state(&req->tty.signal_mask, &req->tty.signal_ignored);
    const int64_t status =
        lpr_termd_call(op, page_fd, page, sizeof(*req), out_result);
    lpr_destroy_tty_wire_page(page_fd, page);
    return status;
}

int64_t lpr_termd_transfer_dup_handle(
    uint64_t handle,
    int lease_fd,
    uint64_t *out_handle)
{
    if (out_handle == 0 || lease_fd < 16) {
        return -LPR_LINUX_EINVAL;
    }
    *out_handle = 0;
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    termd_handle_request_t *request =
        (termd_handle_request_t *)lpr_termd_payload(page);
    lpr_memset(request, 0, sizeof(*request));
    request->handle = handle;
    lpr_fill_termd_caller(
        &request->tty.session_id,
        &request->tty.process_id,
        &request->tty.pgrp_id);
    lpr_fill_termd_signal_state(
        &request->tty.signal_mask,
        &request->tty.signal_ignored);
    const int64_t status = lpr_termd_call_with_fd(
        TERMD_OP_HANDLE_DUP,
        page_fd,
        page,
        sizeof(*request),
        lease_fd,
        out_handle);
    lpr_destroy_tty_wire_page(page_fd, page);
    return status == 0 && *out_handle == handle ? 0 :
        (status != 0 ? status : -LPR_LINUX_EIO);
}

int lpr_tty_fd_alloc(uint64_t handle, uint64_t flags, int native_wait_fd)
{
    const int fd = lpr_fd_slot_alloc();
    if (fd < 0) {
        return fd;
    }
    const int control_status = lpr_control_install_fd(
        (uint64_t)fd,
        LPR_FD_OPS_TTY,
        flags,
        handle,
        0);
    if (control_status != 0) {
        return control_status;
    }
    lpr_tty_backend_t *tty = lpr_tty_backend((uint64_t)fd);
    if (tty == 0) {
        lpr_control_close_fd((uint64_t)fd);
        return -LPR_LINUX_EIO;
    }
    tty->active = 1;
    tty->flags = (uint32_t)flags;
    tty->handle = handle;
    tty->wait_fd.raw = native_wait_fd;
    return fd;
}

uint64_t lpr_parse_pts_index(const char *path)
{
    const char prefix[] = "/dev/pts/";
    for (uint64_t i = 0; prefix[i] != 0; i++) {
        if (path[i] != prefix[i]) {
            return UINT64_MAX;
        }
    }
    uint64_t value = 0;
    uint64_t pos = sizeof(prefix) - 1u;
    if (path[pos] == 0) {
        return UINT64_MAX;
    }
    while (path[pos] != 0) {
        if (path[pos] < '0' || path[pos] > '9') {
            return UINT64_MAX;
        }
        value = value * 10u + (uint64_t)(path[pos] - '0');
        pos++;
    }
    return value;
}

uint64_t lpr_parse_hvc_index(const char *path)
{
    const char prefix[] = "/dev/hvc";
    for (uint64_t i = 0; prefix[i] != 0; i++) {
        if (path[i] != prefix[i]) {
            return UINT64_MAX;
        }
    }
    uint64_t value = 0;
    uint64_t pos = sizeof(prefix) - 1u;
    if (path[pos] == 0) {
        return UINT64_MAX;
    }
    while (path[pos] != 0) {
        if (path[pos] < '0' || path[pos] > '9') {
            return UINT64_MAX;
        }
        value = value * 10u + (uint64_t)(path[pos] - '0');
        pos++;
    }
    return value;
}

void lpr_fill_termd_caller(uint64_t *session_id, uint64_t *process_id, uint64_t *pgrp_id)
{
    lpr_linux_process_state_init();
    if (session_id != 0) {
        *session_id = (uint64_t)(uint32_t)lpr_linux_current_sid;
    }
    if (process_id != 0) {
        *process_id = (uint64_t)(uint32_t)lpr_linux_current_pid;
    }
    if (pgrp_id != 0) {
        *pgrp_id = (uint64_t)(uint32_t)lpr_linux_current_pgrp;
    }
}

uint64_t lpr_linux_signal_bit(uint32_t sig)
{
    if (sig == 0 || sig > LPR_LINUX_SIGNAL_MAX) {
        return 0;
    }
    return 1ull << (sig - 1u);
}

void lpr_linux_queue_signal(uint32_t sig)
{
    const uint64_t bit = lpr_linux_signal_bit(sig);
    if (bit != 0) {
        lpr_linux_pending_signal_mask |= bit;
    }
}

int64_t lpr_linux_rt_sigpending(uint64_t set_raw, uint64_t sigsetsize)
{
    if (sigsetsize != sizeof(uint64_t)) {
        return -LPR_LINUX_EINVAL;
    }
    if (set_raw == 0) {
        return -LPR_LINUX_EFAULT;
    }
    *(uint64_t *)(uintptr_t)set_raw =
        lpr_linux_pending_signal_mask |
        __atomic_load_n(
            &lpr_state.signal.signalfd_pending_mask,
            __ATOMIC_ACQUIRE);
    return 0;
}

void lpr_linux_signal_after_fork_child(void)
{
    lpr_signal_thread_state_after_fork_child();
}

int lpr_linux_default_signal_ignored(uint32_t sig)
{
    return sig == LPR_LINUX_SIGCHLD ||
        sig == LPR_LINUX_SIGURG ||
        sig == LPR_LINUX_SIGWINCH ||
        sig == LPR_LINUX_SIGCONT;
}

int lpr_linux_default_signal_stops(uint32_t sig)
{
    return sig == LPR_LINUX_SIGSTOP ||
        sig == LPR_LINUX_SIGTSTP ||
        sig == LPR_LINUX_SIGTTIN ||
        sig == LPR_LINUX_SIGTTOU;
}

void lpr_linux_exit_for_signal(uint32_t sig)
{
    const uint64_t exit_code = 128u + (uint64_t)sig;
    lpr_linux_prepare_process_exit(exit_code);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_PROCESS_EXIT, exit_code);
    for (;;) {
    }
}

uint32_t lpr_linux_first_pending_signal(uint64_t mask)
{
    for (uint32_t sig = 1; sig <= LPR_LINUX_SIGNAL_MAX; sig += 1) {
        if ((mask & lpr_linux_signal_bit(sig)) != 0) {
            return sig;
        }
    }
    return 0;
}

int64_t lpr_linux_dispatch_pending_signals_with_result(int64_t interrupted_result)
{
    if (lpr_linux_signal_dispatching) {
        return 0;
    }
    lpr_linux_signal_dispatching = 1;

    int64_t result = 0;
    for (;;) {
        const uint64_t deliverable = lpr_linux_pending_signal_mask & ~lpr_linux_signal_mask;
        const uint32_t sig = lpr_linux_first_pending_signal(deliverable);
        if (sig == 0) {
            break;
        }
        const uint64_t bit = lpr_linux_signal_bit(sig);
        lpr_linux_pending_signal_mask &= ~bit;

        const lpr_linux_sigaction_record_t action = lpr_linux_sigactions[sig];
        if (action.handler == LPR_LINUX_SIG_IGN ||
            (action.handler == LPR_LINUX_SIG_DFL && lpr_linux_default_signal_ignored(sig)))
        {
            continue;
        }
        if (action.handler == LPR_LINUX_SIG_DFL) {
            if (lpr_linux_default_signal_stops(sig)) {
                result = -LPR_LINUX_EINTR;
                break;
            }
            lpr_linux_exit_for_signal(sig);
        }

        // Converting a queued Linux signal back into a native signal requires
        // a captured Linux return frame.  Without one, DELIVER_PENDING_FRAME
        // cannot consume it and a timer would inject the handler at an
        // arbitrary instruction instead of the next syscall boundary.
        if (lpr_current_linux_user_frame() == 0) {
            lpr_linux_pending_signal_mask |= bit;
            break;
        }

        const int64_t signal_status = lpr_pacha_syscall2(
            PACHAOS_SYSCALL_PROCESS_SIGNAL,
            PACHA_PROCESS_SELF_FD,
            sig);
        if (signal_status != 0) {
            lpr_linux_pending_signal_mask |= bit;
            result = lpr_pacha_status_to_errno(signal_status);
            break;
        }

        lpr_linux_signal_dispatching = 0;
        lpr_linux_deliver_native_pending_frame(interrupted_result);
        return interrupted_result;
    }

    lpr_linux_signal_dispatching = 0;
    return result;
}

int64_t lpr_linux_dispatch_pending_signals(void)
{
    return lpr_linux_dispatch_pending_signals_with_result(-LPR_LINUX_EINTR);
}

void lpr_linux_raise_sigpipe(void)
{
    lpr_linux_queue_signal(LPR_LINUX_SIGPIPE);
}

uint64_t lpr_linux_unblockable_signal_mask(void)
{
    return lpr_linux_signal_bit(LPR_LINUX_SIGKILL) |
        lpr_linux_signal_bit(LPR_LINUX_SIGSTOP);
}

uint64_t lpr_linux_ignored_signal_mask(void)
{
    uint64_t ignored = 0;
    for (uint32_t sig = 1; sig <= LPR_LINUX_SIGNAL_MAX; sig += 1) {
        if (lpr_linux_sigactions[sig].handler == LPR_LINUX_SIG_IGN) {
            ignored |= lpr_linux_signal_bit(sig);
        }
    }
    return ignored;
}

void lpr_fill_termd_signal_state(uint64_t *signal_mask, uint64_t *signal_ignored)
{
    if (signal_mask != 0) {
        *signal_mask = lpr_linux_signal_mask & ~lpr_linux_unblockable_signal_mask();
    }
    if (signal_ignored != 0) {
        *signal_ignored = lpr_linux_ignored_signal_mask();
    }
}

int64_t lpr_tty_open_path(const char *path, uint64_t flags)
{
    if (path == 0 || LPR_TERMD_TTY_ENDPOINT_FD < 16) {
        return -LPR_LINUX_ENOENT;
    }
    uint64_t op = 0;
    uint64_t pts_index = 0;
    if (lpr_strcmp(path, "/dev/ptmx") == 0) {
        op = TERMD_OP_OPEN_PTMX;
    } else if (lpr_strcmp(path, "/dev/tty") == 0) {
        op = TERMD_OP_OPEN_CTTY;
    } else if (lpr_strcmp(path, "/dev/console") == 0) {
        op = TERMD_OP_OPEN_HVC;
        pts_index = 0;
    } else {
        pts_index = lpr_parse_pts_index(path);
        if (pts_index != UINT64_MAX) {
            op = TERMD_OP_OPEN_PTS;
        } else {
            pts_index = lpr_parse_hvc_index(path);
            if (pts_index == UINT64_MAX) {
                return -LPR_LINUX_ENOENT;
            }
            op = TERMD_OP_OPEN_HVC;
        }
    }

    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    termd_open_request_t *open_req = (termd_open_request_t *)lpr_termd_payload(page);
    lpr_memset(open_req, 0, sizeof(*open_req));
    open_req->flags = flags;
    open_req->pts_index = pts_index;
    lpr_fill_termd_caller(
        &open_req->tty.session_id,
        &open_req->tty.process_id,
        &open_req->tty.pgrp_id);
    lpr_fill_termd_signal_state(
        &open_req->tty.signal_mask,
        &open_req->tty.signal_ignored);
    uint64_t handle = 0;
    int native_wait_fd = -1;
    int remote_wait_fd = -1;
    const int wait_status = lpr_native_wait_pair(&native_wait_fd, &remote_wait_fd);
    if (wait_status != 0) {
        lpr_destroy_tty_wire_page(page_fd, page);
        return wait_status;
    }
    const int64_t status = lpr_termd_call_with_fd(
        op, page_fd, page, sizeof(*open_req), remote_wait_fd, &handle);
    lpr_destroy_tty_wire_page(page_fd, page);
    if (status != 0) {
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)native_wait_fd);
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)remote_wait_fd);
        return status;
    }
    const int fd = lpr_tty_fd_alloc(handle, flags, native_wait_fd);
    if (fd < 0) {
        (void)lpr_termd_call_handle(TERMD_OP_HANDLE_CLOSE, handle, 0);
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)native_wait_fd);
        return fd;
    }
    lpr_tty_backend_t *tty = lpr_tty_backend((uint64_t)fd);
    if (tty != 0) {
        if (op == TERMD_OP_OPEN_PTMX) {
            tty->reserved0 = LPR_TTY_BACKEND_PTY_MASTER;
        } else if (op == TERMD_OP_OPEN_PTS) {
            tty->reserved0 = LPR_TTY_BACKEND_PTY_SLAVE;
        }
    }
    return fd;
}
