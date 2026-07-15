#include "../lpr_filed_internal.h"

void lpr_pipe_after_fork_child(void)
{
    /* Native endpoints are inherited by the kernel fd-table clone.  Service
     * handles belong to the child session and must be reacquired once. */
    lpr_reset_fork_child_rpc_state();
    lpr_cwd_init();
    if (lpr_cwd_handle != 0) {
        uint64_t dup_handle = 0;
        const int64_t status = lpr_filed_dup_handle(lpr_cwd_handle, 0, &dup_handle);
        lpr_trace_process_event("fork_dup_cwd", lpr_cwd_handle, dup_handle, status);
        if (status == 0 && dup_handle != 0) {
            lpr_cwd_handle = dup_handle;
        }
    }
    for (uint32_t index = 0; index < lpr_control_fd_table.ofd_count; index++) {
        lpr_ofd_t *ofd = &lpr_control_fd_table.ofds[index];
        if (!ofd->active) {
            continue;
        }
        const uint8_t ops_id = lpr_ofd_ops_id(ofd);
        if (ops_id == LPR_FD_OPS_TTY) {
            lpr_tty_backend_t *tty = lpr_backend_state_from_ofd(ofd);
            uint64_t dup_handle = 0;
            const int64_t status = lpr_termd_call_handle(
                TERMD_OP_HANDLE_DUP,
                tty->handle,
                &dup_handle);
            lpr_trace_process_event("fork_dup_tty", index, dup_handle, status);
            if (status == 0 && dup_handle != 0) {
                tty->handle = dup_handle;
            }
        } else if (ops_id == LPR_FD_OPS_FILED) {
            lpr_filed_backend_t *filed = lpr_backend_state_from_ofd(ofd);
            void *page = 0;
            const int page_fd = lpr_create_wire_page(&page);
            if (page_fd < 0) {
                lpr_trace_process_event("fork_dup_filed_page", index, 0, page_fd);
                continue;
            }
            filed_handle_flags_t *flags = (filed_handle_flags_t *)page;
            lpr_memset(flags, 0, sizeof(*flags));
            flags->handle = filed->handle;
            uint64_t dup_handle = 0;
            const int64_t status = lpr_filed_call(FILED_OP_VFS_DUP, page_fd, 0, &dup_handle);
            lpr_destroy_wire_page(page_fd, page);
            lpr_trace_process_event("fork_dup_filed", index, dup_handle, status);
            if (status == 0 && dup_handle != 0) {
                filed->handle = dup_handle;
            }
        } else if (ops_id == LPR_FD_OPS_DRM) {
            lpr_drm_backend_t *drm = lpr_backend_state_from_ofd(ofd);
            const int64_t status = lpr_drm_dup_handle(drm->handle);
            lpr_trace_process_event("fork_dup_drm", index, drm->handle, status);
        } else if (ops_id == LPR_FD_OPS_INPUT) {
            lpr_input_backend_t *input = lpr_backend_state_from_ofd(ofd);
            const int64_t status = lpr_input_dup_handle(input->handle);
            lpr_trace_process_event("fork_dup_input", index, input->handle, status);
        } else if (ops_id == LPR_FD_OPS_DMABUF) {
            lpr_dmabuf_backend_t *dmabuf = lpr_backend_state_from_ofd(ofd);
            const int64_t status = lpr_drm_prime_ref(DRMD_OP_PRIME_ACQUIRE, dmabuf->token);
            lpr_trace_process_event("fork_acquire_dmabuf", index, dmabuf->token, status);
            if (status != 0) {
                dmabuf->token = 0;
            }
        }
    }
}

void lpr_linux_apply_pending_fork_child(void)
{
    if (lpr_linux_pending_child_pid <= 0) {
        return;
    }
    const int32_t child_pid = lpr_linux_pending_child_pid;
    const int32_t child_ppid = lpr_linux_pending_child_ppid;
    const int32_t child_sid = lpr_linux_pending_child_sid;
    const int32_t child_pgrp = lpr_linux_pending_child_pgrp;
    const uint64_t child_token = lpr_supervisor_pending_child_token;
    lpr_linux_process_state_checked = 1;
    if (child_token != 0) {
        lpr_supervisor_token = child_token;
        lpr_supervisor_enabled = 1;
        (void)lpr_supervisor_call_token(
            LPRS_OP_PROCESS_FORK_CHILD_READY,
            lpr_supervisor_token,
            -1,
            0);
    }
    lpr_linux_current_pid = child_pid;
    lpr_linux_current_ppid = child_ppid;
    lpr_linux_current_sid = child_sid > 0 ? child_sid : child_pid;
    lpr_linux_current_pgrp = child_pgrp > 0 ? child_pgrp : child_pid;
    lpr_linux_pending_child_pid = 0;
    lpr_linux_pending_child_ppid = 0;
    lpr_linux_pending_child_sid = 0;
    lpr_linux_pending_child_pgrp = 0;
    lpr_supervisor_pending_child_token = 0;
    lpr_linux_process_clear_children();
    lpr_linux_signal_after_fork_child();
    lpr_thread_after_fork_child();
    lpr_pipe_after_fork_child();
    lpr_trace_process_event(
        "fork_child_state",
        (uint64_t)(uint32_t)child_pid,
        (uint64_t)(uint32_t)child_ppid,
        0);
}

int64_t lpr_linux_pipe2(uint64_t fds_raw, uint64_t flags)
{
    const uint64_t known_flags = LPR_LINUX_O_CLOEXEC | LPR_LINUX_O_NONBLOCK;
    if (fds_raw == 0) {
        return -LPR_LINUX_EFAULT;
    }
    if ((flags & ~known_flags) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    uint64_t pair[2] = {0, 0};
    const int64_t status = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_PIPE_CREATE,
        (uint64_t)(uintptr_t)pair,
        lpr_pipe_flags_to_pacha(flags));
    if (status != 0) {
        return lpr_pacha_status_to_errno(status);
    }
    struct pacha_fd_info read_info;
    struct pacha_fd_info write_info;
    if (!lpr_native_fd_info(pair[0], &read_info) || read_info.kind != PACHA_FD_KIND_PIPE ||
        !lpr_native_fd_info(pair[1], &write_info) || write_info.kind != PACHA_FD_KIND_PIPE)
    {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[0]);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[1]);
        return -LPR_LINUX_EIO;
    }
    const int read_fd = lpr_fd_slot_alloc_from(3);
    if (read_fd < 0) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[0]);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[1]);
        return read_fd;
    }
    int install_status = lpr_pipe_track_native_fd(
        (uint64_t)(uint32_t)read_fd,
        pair[0],
        &read_info);
    if (install_status != 0) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[0]);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[1]);
        return install_status;
    }
    const int write_fd = lpr_fd_slot_alloc_from(3);
    if (write_fd < 0) {
        lpr_control_close_fd((uint64_t)(uint32_t)read_fd);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[1]);
        return write_fd;
    }
    install_status = lpr_pipe_track_native_fd(
        (uint64_t)(uint32_t)write_fd,
        pair[1],
        &write_info);
    if (install_status != 0) {
        lpr_control_close_fd((uint64_t)(uint32_t)read_fd);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[1]);
        return install_status;
    }
    int *fds = (int *)(uintptr_t)fds_raw;
    fds[0] = read_fd;
    fds[1] = write_fd;
    return 0;
}

int64_t lpr_linux_eventfd2(uint64_t initval, uint64_t flags)
{
    const uint64_t known_flags = LPR_LINUX_O_CLOEXEC | LPR_LINUX_O_NONBLOCK;
    if ((flags & ~known_flags) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    const int fd = lpr_fd_slot_alloc();
    if (fd < 0) {
        return fd;
    }
    const int status = lpr_control_install_fd(
        (uint64_t)(uint32_t)fd,
        LPR_FD_OPS_EVENT,
        flags,
        0,
        initval);
    if (status != 0) {
        return status;
    }
    lpr_event_backend_t *event = lpr_event_backend((uint64_t)(uint32_t)fd);
    if (event == 0) {
        lpr_control_close_fd((uint64_t)(uint32_t)fd);
        return -LPR_LINUX_EIO;
    }
    event->subtype = LPR_EVENT_BACKEND_EVENTFD;
    return fd;
}

int64_t lpr_linux_dup_into(uint64_t fd, int target_fd, uint64_t min_fd, uint64_t cloexec)
{
    if (fd > LPR_LINUX_FD_MAX || lpr_control_require_fd(fd) != 0) {
        return -LPR_LINUX_EBADF;
    }
    if (target_fd < 0) {
        if (min_fd > LPR_LINUX_FD_MAX) {
            return -LPR_LINUX_EINVAL;
        }
        const int new_fd = lpr_fd_slot_alloc_from(min_fd);
        if (new_fd < 0) {
            return new_fd;
        }
        if (lpr_control_dup_fd(fd, (uint64_t)(uint32_t)new_fd, cloexec) != 0) {
            return -LPR_LINUX_EMFILE;
        }
        return new_fd;
    }
    const uint64_t new_fd = (uint64_t)(uint32_t)target_fd;
    if (target_fd < 0 || new_fd > LPR_LINUX_FD_MAX) {
        return -LPR_LINUX_EINVAL;
    }
    if (fd == new_fd) {
        return (int64_t)new_fd;
    }
    const int ensure_status = lpr_fd_table_ensure_fd(new_fd);
    if (ensure_status != 0) {
        return ensure_status;
    }
    if (lpr_control_fd_active(new_fd)) {
        const int64_t close_status = lpr_linux_close(new_fd);
        if (close_status != 0) {
            return close_status;
        }
    }
    return lpr_control_dup_fd(fd, new_fd, cloexec) == 0 ?
        (int64_t)new_fd : -LPR_LINUX_EBADF;
}

int64_t lpr_linux_dup(uint64_t fd, uint64_t min_fd, uint64_t cloexec)
{
    return lpr_linux_dup_into(fd, -1, min_fd, cloexec);
}

int64_t lpr_linux_dup2(uint64_t old_fd, uint64_t new_fd, uint64_t flags)
{
    const uint64_t known_flags = LPR_LINUX_O_CLOEXEC;
    if ((flags & ~known_flags) != 0 || new_fd > LPR_LINUX_FD_MAX) {
        return -LPR_LINUX_EINVAL;
    }
    if (old_fd == new_fd) {
        return flags == 0 && lpr_control_require_fd(old_fd) == 0 ?
            (int64_t)new_fd : -LPR_LINUX_EINVAL;
    }
    return lpr_linux_dup_into(
        old_fd,
        (int)new_fd,
        0,
        (flags & LPR_LINUX_O_CLOEXEC) != 0);
}
