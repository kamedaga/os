#include "../lpr_filed_internal.h"
#include "filed/identity.h"
#include "session.h"
#include "../lpr_common/boot_diagnostic.h"

static int lpr_filed_adopt_on_connection(uint64_t lease_handle, pacha_fd_t lease_fd, uint64_t *handle);

int64_t lpr_filed_fd_binding(uint64_t fd)
{
    /* Local TTY/pipe/device operations must not acquire filesystem authority.
     * Only a live FILED backend needs connection-relative handle adoption. */
    return lpr_fd_is_filed(fd) ? lpr_filed_endpoint_ready() : 0;
}

int64_t lpr_filed_endpoint_ready(void)
{
    if (lpr_fs_session_terminal()) return -LPR_LINUX_EIO;
    lpr_boot_diagnostic_fs_begin();
    if (__atomic_load_n(&lpr_filed_endpoint_checked, __ATOMIC_ACQUIRE) > 0) return 0;
    lpr_boot_diagnostic_fs_emit("fs.endpoint.lock", lpr_filed_endpoint_checked,
        lpr_filed_client_fd, 0, 0, 0);
    lpr_state_lock(&lpr_state.filed_rpc.connection_lock);
    lpr_boot_diagnostic_fs_emit("fs.endpoint.acquired", lpr_filed_endpoint_checked,
        lpr_filed_client_fd, 0, 0, 0);
    int64_t status = 0;
    if (lpr_filed_endpoint_checked > 0) goto done;
    if (lpr_filed_endpoint_checked < 0) {
        status = -LPR_LINUX_ENOSYS;
        goto done;
    }
    lpr_linux_process_state_init();
    if (!lpr_supervisor_enabled) { status = -LPR_LINUX_ENOSYS; goto done; }
    void *page = 0;
    const pacha_fd_t page_fd = lpr_create_standalone_wire_page(&page);
    if (page_fd < 16) { status = page_fd; goto done; }
    lpr_boot_diagnostic_fs_emit("fs.session.enter", LPRS_OP_PROCESS_FILED_SESSION,
        page_fd, 0, 0, 0);
    status = lpr_process_client_service_session(LPRS_OP_PROCESS_FILED_SESSION,
        &lpr_request_id, lpr_pacha_status_to_errno, lpr_supervisor_token,
        page_fd, page, &lpr_filed_client_id, &lpr_filed_client_fd);
    lpr_boot_diagnostic_fs_emit("fs.session.return", lpr_filed_client_id,
        lpr_filed_client_fd, 0, status, 0);
    lpr_destroy_standalone_wire_page(page_fd, page);
    if (status) goto done;
    /* Manifest/fork leases are capabilities, not authority inferred from the
     * serialized handle numbers. Bind each to this new process connection. */
    lpr_fd_table_lock(&lpr_control_fd_table);
    for (uint32_t i = 0; i < lpr_control_fd_table.backend_count; i++) {
        lpr_backend_record_t *record = &lpr_control_fd_table.backends[i];
        if (!record->active || record->ops_id != LPR_FD_OPS_FILED) continue;
        lpr_filed_backend_t *file = record->state;
        if ((file->reserved2 & LPR_BACKEND_TRANSFER_LEASE) && file->lease_fd.raw >= 16) {
            if (!file->lease_handle) file->lease_handle = file->handle;
            status = lpr_filed_adopt_on_connection(file->lease_handle,
                file->lease_fd.raw, &file->handle);
            if (status) break;
        }
    }
    lpr_fd_table_unlock(&lpr_control_fd_table);
    if (!status && lpr_cwd_lease_fd >= 16) {
        lpr_cwd_init();
        if (!lpr_cwd_lease_handle) lpr_cwd_lease_handle = lpr_cwd_handle;
        status = lpr_filed_adopt_on_connection(lpr_cwd_lease_handle,
            lpr_cwd_lease_fd, &lpr_cwd_handle);
    }
    lpr_cwd_init();
    if (status) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)lpr_filed_client_fd);
        lpr_filed_client_fd = -1;
    }
    __atomic_store_n(&lpr_filed_endpoint_checked, status ? -1 : 1, __ATOMIC_RELEASE);
done:
    lpr_state_unlock(&lpr_state.filed_rpc.connection_lock);
    lpr_boot_diagnostic_fs_emit("fs.endpoint.return", lpr_filed_client_id,
        lpr_filed_client_fd, 0, status, 0);
    return status;
}

int lpr_filed_lease_pair(pacha_fd_t *local, pacha_fd_t *remote)
{
    uint64_t pair[2] = {0};
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_SET_FLAGS |
        PACHA_FD_RIGHT_CALL | PACHA_FD_RIGHT_SEND | PACHA_FD_RIGHT_RECV |
        PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL;
    int64_t status = lpr_pacha_syscall3(PACHAOS_SYSCALL_IPC_CHANNEL_CREATE,
        (uint64_t)(uintptr_t)pair, rights, 0);
    if (status) return (int)lpr_pacha_status_to_errno(status);
    *local = (pacha_fd_t)pair[0];
    *remote = (pacha_fd_t)pair[1];
    return 0;
}

static int lpr_filed_adopt_on_connection(uint64_t lease_handle, pacha_fd_t lease_fd, uint64_t *handle)
{
    struct pacha_ipc_msg request = { .word0 = FILED_LEASE_MAGIC, .word1 = lease_handle,
        .word2 = lpr_filed_client_id, .word3 = lease_handle };
    lpr_boot_diagnostic_fs_emit("fs.adopt.call", lease_handle, lease_fd,
        lpr_filed_client_id, 0, 0);
    struct pacha_ipc_msg reply = {0};
    int status = (int)lpr_native_ipc_call_wait((uint64_t)lease_fd, &request, &reply, NULL);
    if (status) return (int)lpr_pacha_status_to_errno(status);
    if (reply.word0 != FILED_LEASE_MAGIC || reply.word3 != lease_handle ||
        (!reply.word1 && !reply.word2))
        return -LPR_LINUX_EIO;
    if (!reply.word1) *handle = reply.word2;
    return (int)(int64_t)reply.word1;
}

int lpr_filed_adopt(uint64_t *handle, pacha_fd_t lease_fd)
{
    int status = (int)lpr_filed_endpoint_ready();
    return status ? status : lpr_filed_adopt_on_connection(*handle, lease_fd, handle);
}

pacha_fd_t lpr_create_tty_wire_page(void **out_page)
{
    if (out_page == 0) {
        return -LPR_LINUX_EINVAL;
    }
    lpr_state_lock(&lpr_state.termd_rpc.lock_word);
    if (lpr_tty_wire_page_fd >= 16 && lpr_tty_wire_page != 0 && !lpr_tty_wire_page_busy) {
        lpr_tty_wire_page_busy = 1;
        *out_page = lpr_tty_wire_page;
        return lpr_tty_wire_page_fd;
    }
    const uint64_t rights =
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    const int64_t fd = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_VMO_CREATE,
        TERMD_PAGE_BYTES,
        rights,
        0);
    if (fd < 16) {
        const int error = (int)lpr_pacha_status_to_errno(fd);
        lpr_state_unlock(&lpr_state.termd_rpc.lock_word);
        return error;
    }
    const int64_t mapped = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_MMAP,
        (uint64_t)(uint32_t)fd,
        0,
        TERMD_PAGE_BYTES,
        PACHAOS_PROT_READ | PACHAOS_PROT_WRITE,
        PACHAOS_MMAP_SHARED,
        0);
    if (mapped < 4096) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)fd);
        const int error = (int)lpr_pacha_status_to_errno(mapped);
        lpr_state_unlock(&lpr_state.termd_rpc.lock_word);
        return error;
    }
    *out_page = (void *)(uintptr_t)mapped;
    if (!lpr_tty_wire_page_busy && lpr_tty_wire_page_fd < 16 && lpr_tty_wire_page == 0) {
        lpr_tty_wire_page_fd = (pacha_fd_t)fd;
        lpr_tty_wire_page = *out_page;
        lpr_tty_wire_page_busy = 1;
    }
    return (pacha_fd_t)fd;
}

void lpr_destroy_tty_wire_page(pacha_fd_t fd, void *page)
{
    if (fd == lpr_tty_wire_page_fd && page == lpr_tty_wire_page) {
        lpr_tty_wire_page_busy = 0;
        lpr_state_unlock(&lpr_state.termd_rpc.lock_word);
        return;
    }
    if (page != 0) {
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)(uintptr_t)page, TERMD_PAGE_BYTES);
    }
    if (fd >= 16) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)fd);
    }
    lpr_state_unlock(&lpr_state.termd_rpc.lock_word);
}

void lpr_reset_fork_child_rpc_state(void)
{
    lpr_fs_session_after_fork_child();
    /* PRIVATE control capability was not inherited. Its old numeric slot
     * may now name another child capability; never close it by stale number. */
    lpr_filed_client_fd = -1;
    lpr_filed_client_id = 0;
    lpr_filed_endpoint_checked = 0;
    lpr_state.filed_rpc.connection_lock = 0;
    lpr_state.filed_rpc.lock_word = 0;
    lpr_state.filed_rpc.readv_lock_word = 0;
    lpr_state.termd_rpc.lock_word = 0;
    /* Fork copies the process-local RPC lock, but this VMO mapping is shared.
     * Keeping the parent's cached page lets independent parent/child requests
     * overwrite an in-flight reply (including exec's TTY transfer response).
     * Drop only the child's inherited ownership before any child RPC; the
     * parent retains its mapping/capability and the child allocates its own. */
    const pacha_fd_t inherited_tty_fd = lpr_tty_wire_page_fd;
    void *const inherited_tty_page = lpr_tty_wire_page;
    lpr_tty_wire_page_fd = -1;
    lpr_tty_wire_page = 0;
    lpr_tty_wire_page_busy = 0;
    lpr_destroy_standalone_wire_page(inherited_tty_fd, inherited_tty_page);
    lpr_state.netd_rpc.lock_word = 0;
    lpr_state.netd_rpc.endpoint_checked = 0;
    lpr_netd_page_pool_after_fork_child();


}

pacha_fd_t lpr_create_standalone_wire_page(void **out_page)
{
    if (out_page == 0) {
        return -LPR_LINUX_EINVAL;
    }
    *out_page = 0;
    const uint64_t rights =
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    const int64_t fd = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_VMO_CREATE,
        LPR_CONTROL_PAGE_BYTES,
        rights,
        0);
    if (fd < 16) {
        return (int)lpr_pacha_status_to_errno(fd);
    }
    const int64_t mapped = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_MMAP,
        (uint64_t)(uint32_t)fd,
        0,
        LPR_CONTROL_PAGE_BYTES,
        PACHAOS_PROT_READ | PACHAOS_PROT_WRITE,
        PACHAOS_MMAP_SHARED,
        0);
    if (mapped < 4096) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)fd);
        return (int)lpr_pacha_status_to_errno(mapped);
    }
    *out_page = (void *)(uintptr_t)mapped;
    return (pacha_fd_t)fd;
}

void lpr_destroy_standalone_wire_page(pacha_fd_t fd, void *page)
{
    if (page != 0) {
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)(uintptr_t)page, LPR_CONTROL_PAGE_BYTES);
    }
    if (fd >= 16) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)fd);
    }
}

void *lpr_supervisor_payload(void *page)
{
    return lpr_process_client_payload(page);
}

int64_t lpr_supervisor_call(
    uint32_t op,
    pacha_fd_t page_fd,
    void *page,
    uint32_t payload_size,
    pacha_fd_t transfer_fd,
    uint64_t *out_result)
{
    return lpr_process_client_call(
        &lpr_request_id,
        lpr_pacha_status_to_errno,
        op,
        page_fd,
        page,
        payload_size,
        transfer_fd,
        out_result);
}

int64_t lpr_supervisor_call_token(
    uint32_t op,
    uint64_t token,
    pacha_fd_t transfer_fd,
    uint64_t *out_result)
{
    return lpr_process_client_call_token(
        &lpr_request_id,
        lpr_pacha_status_to_errno,
        lpr_create_standalone_wire_page,
        lpr_destroy_standalone_wire_page,
        op,
        token,
        transfer_fd,
        out_result);
}

int64_t lpr_supervisor_kill_pid(int32_t pid, uint32_t sig, uint64_t *out_delivered)
{
    if (!lpr_supervisor_enabled || lpr_supervisor_token == 0 || sig > LPR_LINUX_SIGNAL_MAX) {
        return -LPR_LINUX_EINVAL;
    }
    void *page = 0;
    const pacha_fd_t page_fd = lpr_create_standalone_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    lpr_memset(page, 0, PACHA_SERVICE_PAGE_BYTES);
    lprs_kill_t *kill_req = (lprs_kill_t *)lpr_supervisor_payload(page);
    kill_req->token = lpr_supervisor_token;
    kill_req->pid = pid;
    kill_req->signal = sig;
    const int64_t status = lpr_supervisor_call(
        LPRS_OP_SIGNAL_KILL,
        page_fd,
        page,
        sizeof(*kill_req),
        -1,
        0);
    if (status == 0 && out_delivered != 0) {
        *out_delivered = kill_req->delivered;
    }
    lpr_destroy_standalone_wire_page(page_fd, page);
    return status;
}

int lpr_supervisor_get_owner(lprs_process_state_t *out_state, pacha_fd_t *out_process_fd)
{
    if (out_state == 0 || out_process_fd == 0 || lpr_supervisor_token == 0) {
        return -LPR_LINUX_EINVAL;
    }
    *out_process_fd = -1;
    void *page = 0;
    const pacha_fd_t page_fd = lpr_create_standalone_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    lpr_memset(page, 0, PACHA_SERVICE_PAGE_BYTES);
    lprs_token_request_t *req = (lprs_token_request_t *)lpr_supervisor_payload(page);
    req->token = lpr_supervisor_token;
    const int64_t status = lpr_process_client_call_with_reply_fd(
        &lpr_request_id,
        lpr_pacha_status_to_errno,
        LPRS_OP_PROCESS_GET_STATE,
        page_fd,
        page,
        sizeof(*req),
        -1,
        0,
        out_process_fd);
    if (status == 0) {
        const pacha_service_envelope_t *header = page;
        if (header->reply_payload_size != sizeof(*out_state)) {
            if (*out_process_fd >= 16) {
                (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)*out_process_fd);
                *out_process_fd = -1;
            }
            lpr_destroy_standalone_wire_page(page_fd, page);
            return -LPR_LINUX_EIO;
        }
        lpr_memcpy(out_state, lpr_supervisor_payload(page), sizeof(*out_state));
    }
    lpr_destroy_standalone_wire_page(page_fd, page);
    return status == 0 ? 0 : (int)status;
}

int lpr_supervisor_get_state(lprs_process_state_t *out_state)
{
    pacha_fd_t process_fd = -1;
    const int status = lpr_supervisor_get_owner(out_state, &process_fd);
    if (process_fd >= 16)
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)process_fd);
    return status;
}

int lpr_supervisor_list_processes(
    uint64_t offset,
    uint64_t *pids,
    uint64_t capacity,
    uint64_t *out_count)
{
    if (out_count == 0 || (pids == 0 && capacity != 0) ||
        !lpr_supervisor_enabled || lpr_supervisor_token == 0)
    {
        return -LPR_LINUX_EINVAL;
    }
    if (capacity > LPRS_PROCESS_LIST_CAPACITY) {
        capacity = LPRS_PROCESS_LIST_CAPACITY;
    }
    *out_count = 0;

    void *page = 0;
    const pacha_fd_t page_fd = lpr_create_standalone_wire_page(&page);
    if (page_fd < 0) return page_fd;
    lpr_memset(page, 0, PACHA_SERVICE_PAGE_BYTES);
    lprs_process_list_t *list =
        (lprs_process_list_t *)lpr_supervisor_payload(page);
    list->token = lpr_supervisor_token;
    list->offset = offset;
    list->capacity = capacity;
    const int64_t raw_status = lpr_supervisor_call(
        LPRS_OP_PROCESS_LIST,
        page_fd,
        page,
        sizeof(*list),
        -1,
        0);
    if (raw_status == 0) {
        uint64_t count = list->count;
        if (count > capacity) count = capacity;
        if (count != 0) {
            lpr_memcpy(pids, list->pids, (size_t)(count * sizeof(pids[0])));
        }
        *out_count = count;
    }
    lpr_destroy_standalone_wire_page(page_fd, page);
    return raw_status == 0 ? 0 : (int)raw_status;
}

int lpr_supervisor_set_comm(const char *path)
{
    if (path == 0 || !lpr_supervisor_enabled || lpr_supervisor_token == 0) {
        return -LPR_LINUX_EINVAL;
    }
    const uint64_t length = (uint64_t)lpr_strnlen(path, LPR_LINUX_PATH_MAX);
    if (length == 0) return -LPR_LINUX_EINVAL;

    /* comm is the last path component, the way Linux derives it. */
    uint64_t base = length;
    while (base > 0 && path[base - 1u] != '/') base -= 1u;

    void *page = 0;
    const pacha_fd_t page_fd = lpr_create_standalone_wire_page(&page);
    if (page_fd < 0) return page_fd;
    lpr_memset(page, 0, PACHA_SERVICE_PAGE_BYTES);
    lprs_process_set_comm_t *req =
        (lprs_process_set_comm_t *)lpr_supervisor_payload(page);
    req->token = lpr_supervisor_token;

    uint64_t comm_length = length - base;
    if (comm_length > sizeof(req->comm) - 1u) comm_length = sizeof(req->comm) - 1u;
    lpr_memcpy(req->comm, path + base, (size_t)comm_length);
    req->comm[comm_length] = '\0';

    uint64_t cmdline_length = length;
    if (cmdline_length > sizeof(req->cmdline) - 1u)
        cmdline_length = sizeof(req->cmdline) - 1u;
    lpr_memcpy(req->cmdline, path, (size_t)cmdline_length);
    req->cmdline[cmdline_length] = '\0';

    const int64_t raw_status = lpr_supervisor_call(
        LPRS_OP_PROCESS_SET_COMM,
        page_fd,
        page,
        sizeof(*req),
        -1,
        0);
    lpr_destroy_standalone_wire_page(page_fd, page);
    return raw_status == 0 ? 0 : (int)raw_status;
}

/* Hands the supervisor a page this process already owns and keeps writing to.
 * The page travels as a transfer descriptor, which is the same path every
 * other supervisor call uses, so nothing new has to work for the attach to
 * succeed.  The reverse arrangement, mapping a page the supervisor owns, was
 * tried first and left the system unable to reach a shell. */
int lpr_supervisor_diag_attach(void)
{
    if (!lpr_supervisor_enabled || lpr_supervisor_token == 0) {
        return -LPR_LINUX_EINVAL;
    }
    if (lpr_diag_slot != 0) return 0;

    void *diag_page = 0;
    const pacha_fd_t diag_fd = lpr_create_standalone_wire_page(&diag_page);
    if (diag_fd < 0) return diag_fd;
    lpr_memset(diag_page, 0, PACHA_SERVICE_PAGE_BYTES);
    lprs_diag_slot_t *slot = (lprs_diag_slot_t *)diag_page;
    slot->syscall_nr = LPRS_DIAG_SYSCALL_NONE;

    void *page = 0;
    const pacha_fd_t page_fd = lpr_create_standalone_wire_page(&page);
    if (page_fd < 0) {
        lpr_destroy_standalone_wire_page(diag_fd, diag_page);
        return page_fd;
    }
    lpr_memset(page, 0, PACHA_SERVICE_PAGE_BYTES);
    lprs_diag_attach_t *req =
        (lprs_diag_attach_t *)lpr_supervisor_payload(page);
    req->token = lpr_supervisor_token;
    /* The supervisor has to map this page to read it, and read is all it ever
     * needs: the process itself is the only writer. */
    const int64_t status = lpr_process_client_call_with_transfer_rights(
        &lpr_request_id,
        lpr_pacha_status_to_errno,
        LPRS_OP_PROCESS_DIAG_ATTACH,
        page_fd,
        page,
        sizeof(*req),
        diag_fd,
        /* A subset of what the wire page itself holds: a transfer cannot grant
         * a right the sender does not have, and asking for one it lacks fails
         * the whole call.  Write access is deliberately not passed on. */
        PACHA_FD_RIGHT_TRANSFER |
            PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_MAP_READ,
        0,
        0);
    lpr_destroy_standalone_wire_page(page_fd, page);
    if (status != 0) {
        lpr_destroy_standalone_wire_page(diag_fd, diag_page);
        return (int)status;
    }
    /* The supervisor holds the descriptor now.  This mapping stays for the
     * life of the process and is what the syscall hooks write into. */
    lpr_diag_slot = slot;
    return 0;
}

int lpr_supervisor_query_process(uint64_t pid, lprs_process_query_t *out)
{
    if (out == 0 || pid == 0 || !lpr_supervisor_enabled ||
        lpr_supervisor_token == 0)
    {
        return -LPR_LINUX_EINVAL;
    }
    void *page = 0;
    const pacha_fd_t page_fd = lpr_create_standalone_wire_page(&page);
    if (page_fd < 0) return page_fd;
    lpr_memset(page, 0, PACHA_SERVICE_PAGE_BYTES);
    lprs_process_query_t *req =
        (lprs_process_query_t *)lpr_supervisor_payload(page);
    req->token = lpr_supervisor_token;
    req->pid = pid;
    const int64_t raw_status = lpr_supervisor_call(
        LPRS_OP_PROCESS_QUERY,
        page_fd,
        page,
        sizeof(*req),
        -1,
        0);
    if (raw_status == 0) {
        lpr_memcpy(out, req, sizeof(*out));
    }
    lpr_destroy_standalone_wire_page(page_fd, page);
    return raw_status == 0 ? 0 : (int)raw_status;
}

uint64_t lpr_page_align_up(uint64_t value)
{
    const uint64_t mask = 4095ull;
    if (value > UINT64_MAX - mask) {
        return 0;
    }
    return (value + mask) & ~mask;
}

uint64_t lpr_scatter_iov(
    const lpr_linux_iovec_t *iov,
    uint64_t iov_count,
    const unsigned char *src,
    uint64_t length)
{
    uint64_t copied = 0;
    for (uint64_t i = 0; i < iov_count && copied < length; i += 1) {
        if (iov[i].len == 0) {
            continue;
        }
        uint64_t chunk = iov[i].len;
        if (chunk > length - copied) {
            chunk = length - copied;
        }
        lpr_memcpy((void *)(uintptr_t)iov[i].base, src + copied, (size_t)chunk);
        copied += chunk;
    }
    return copied;
}
