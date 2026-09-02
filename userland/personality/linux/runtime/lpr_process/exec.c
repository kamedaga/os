#include "../lpr_filed_internal.h"

enum { LPR_EXEC_BACKEND_SNAPSHOT_BYTES = 256u };

int64_t lpr_linux_wait_process_fd(uint64_t process_fd, uint64_t *out_exit_code)
{
    if (process_fd < 16) {
        return -LPR_LINUX_ECHILD;
    }
    lpr_trace_process_event("wait_begin", process_fd, 0, 0);
    lpr_pacha_process_status_t st;
    for (;;) {
        lpr_memset(&st, 0, sizeof(st));
        const int64_t wait_status = lpr_pacha_syscall2(
            PACHA_PROCESS_SYSCALL_WAIT,
            process_fd,
            (uint64_t)(uintptr_t)&st);
        if (wait_status == 0) {
            if (out_exit_code != 0) {
                *out_exit_code = st.exit_code & 0xffu;
            }
            lpr_trace_process_event("wait_end", process_fd, st.exit_code & 0xffu, 0);
            return 0;
        }
        const int64_t errno_status = lpr_pacha_status_to_errno(wait_status);
        if (errno_status != -LPR_LINUX_EAGAIN) {
            lpr_trace_process_event("wait_error", process_fd, 0, errno_status);
            return errno_status;
        }
        lpr_wait_graph_t graph;
        lpr_wait_deadline_t deadline;
        lpr_wait_graph_init(&graph);
        int64_t fd_wait_status = lpr_wait_graph_add_native(
            &graph, (int)(uint32_t)process_fd, PACHA_FD_EVENT_READABLE);
        if (fd_wait_status == 0)
            fd_wait_status = lpr_wait_deadline_init(&deadline, -1);
        if (fd_wait_status == 0)
            fd_wait_status = lpr_wait_graph_block(&graph, &deadline);
        if (fd_wait_status != 0) {
            lpr_trace_process_event("wait_error", process_fd, 0, fd_wait_status);
            return fd_wait_status;
        }
    }
}

int64_t lpr_linux_try_wait_process_fd(uint64_t process_fd, uint64_t *out_exit_code)
{
    if (process_fd < 16) {
        return -LPR_LINUX_ECHILD;
    }
    lpr_pacha_process_status_t st;
    lpr_memset(&st, 0, sizeof(st));
    const int64_t wait_status = lpr_pacha_syscall2(
        PACHA_PROCESS_SYSCALL_WAIT,
        process_fd,
        (uint64_t)(uintptr_t)&st);
    if (wait_status == 0) {
        if (out_exit_code != 0) {
            *out_exit_code = st.exit_code & 0xffu;
        }
        return 0;
    }
    return lpr_pacha_status_to_errno(wait_status);
}

int lpr_exec_add_string(filed_exec_path_t *exec, filed_exec_string_ref_t *ref, const char *value)
{
    if (exec == 0 || ref == 0 || value == 0) {
        return -LPR_LINUX_EFAULT;
    }
    const uint64_t length = (uint64_t)lpr_strnlen(value, FILED_EXEC_STRING_BYTES) + 1u;
    if (length == 0 || length > UINT16_MAX) {
        return -LPR_LINUX_E2BIG;
    }
    if (exec->string_bytes + length > FILED_EXEC_STRING_BYTES) {
        return -LPR_LINUX_E2BIG;
    }
    ref->offset = (uint16_t)exec->string_bytes;
    ref->length = (uint16_t)length;
    lpr_memcpy(exec->strings + exec->string_bytes, value, (size_t)length);
    exec->string_bytes += length;
    return 0;
}

int lpr_exec_copy_string_vector(
    filed_exec_path_t *exec,
    filed_exec_string_ref_t *refs,
    uint64_t max_refs,
    uint64_t vector_raw,
    uint64_t *out_count)
{
    if (out_count == 0) {
        return -LPR_LINUX_EFAULT;
    }
    *out_count = 0;
    if (vector_raw == 0) {
        return 0;
    }
    const char *const *vector = (const char *const *)(uintptr_t)vector_raw;
    uint64_t count = 0;
    while (count < max_refs) {
        const char *value = vector[count];
        if (value == 0) {
            *out_count = count;
            return 0;
        }
        const int status = lpr_exec_add_string(exec, &refs[count], value);
        if (status != 0) {
            return status;
        }
        count++;
    }
    if (vector[count] != 0) {
        return -LPR_LINUX_E2BIG;
    }
    *out_count = count;
    return 0;
}

uint64_t lpr_align_up_4096(uint64_t value)
{
    const uint64_t mask = 4096ull - 1ull;
    if (value > UINT64_MAX - mask) {
        return 0;
    }
    return (value + mask) & ~mask;
}

int lpr_exec_local_fd_active(uint64_t fd)
{
    return lpr_fd_is_filed(fd) ||
        lpr_linux_tty_fd_active(fd) ||
        lpr_linux_drm_fd_active(fd) ||
        lpr_linux_input_fd_active(fd) ||
        lpr_linux_dmabuf_fd_active(fd) ||
        lpr_linux_sync_file_fd_active(fd) ||
        lpr_pipe_fd_is_active(fd) ||
        lpr_linux_inotify_active(fd) ||
        lpr_linux_eventfd_active(fd) ||
        lpr_linux_socket_fd_active(fd) ||
        lpr_linux_epoll_fd_active(fd);
}

static int lpr_exec_local_fd_preserve_unlocked(uint64_t fd)
{
    if (fd >= lpr_control_fd_table.entry_count) {
        return 0;
    }
    const lpr_fd_entry_t *slot = &lpr_control_fd_table.entries[fd];
    if (!slot->active || slot->ofd_index >= lpr_control_fd_table.ofd_count) {
        return 0;
    }
    const lpr_ofd_t *file = &lpr_control_fd_table.ofds[slot->ofd_index];
    if (lpr_ofd_ops_id(file) == LPR_FD_OPS_EPOLL) {
        return 0;
    }
    return file->active &&
        lpr_ofd_ops_id(file) != LPR_FD_OPS_NONE &&
        (slot->fd_flags & LPR_FD_ENTRY_CLOEXEC) == 0;
}

static int lpr_fork_local_fd_preserve_unlocked(uint64_t fd)
{
    if (fd >= lpr_control_fd_table.entry_count) return 0;
    const lpr_fd_entry_t *entry = &lpr_control_fd_table.entries[fd];
    if (!entry->active || entry->ofd_index >= lpr_control_fd_table.ofd_count)
        return 0;
    const lpr_ofd_t *ofd = &lpr_control_fd_table.ofds[entry->ofd_index];
    return ofd->active && lpr_ofd_ops_id(ofd) != LPR_FD_OPS_NONE;
}

int lpr_exec_local_fd_preserve(uint64_t fd, int *out_preserve)
{
    if (out_preserve == 0) {
        return -LPR_LINUX_EFAULT;
    }
    *out_preserve = 0;
    lpr_fd_arrays_init();
    lpr_fd_table_lock(&lpr_control_fd_table);
    *out_preserve = lpr_exec_local_fd_preserve_unlocked(fd);
    lpr_fd_table_unlock(&lpr_control_fd_table);
    return 0;
}

int lpr_count_exec_local_fds(uint64_t *out_count)
{
    if (out_count == 0) {
        return -LPR_LINUX_EFAULT;
    }
    lpr_fd_arrays_init();
    lpr_fd_table_lock(&lpr_control_fd_table);
    uint64_t count = 0;
    for (uint64_t fd = 0; fd < lpr_fd_table_capacity; fd += 1) {
        if (lpr_exec_local_fd_preserve_unlocked(fd)) {
            count++;
        }
    }
    *out_count = count;
    lpr_fd_table_unlock(&lpr_control_fd_table);
    return 0;
}

static uint64_t lpr_exec_backend_record_bytes(uint8_t ops_id)
{
    switch (ops_id) {
    case LPR_FD_OPS_FILED: return sizeof(lpr_filed_backend_t);
    case LPR_FD_OPS_DEVICE: return sizeof(lpr_device_backend_t);
    case LPR_FD_OPS_TTY: return sizeof(lpr_tty_backend_t);
    case LPR_FD_OPS_DRM: return sizeof(lpr_drm_backend_t);
    case LPR_FD_OPS_INPUT: return sizeof(lpr_input_backend_t);
    case LPR_FD_OPS_PIPE: return sizeof(lpr_pipe_backend_t);
    case LPR_FD_OPS_EVENT: return sizeof(lpr_event_backend_t);
    case LPR_FD_OPS_SOCKET: return sizeof(lpr_socket_backend_t);
    case LPR_FD_OPS_EPOLL: return sizeof(lpr_epoll_backend_t);
    case LPR_FD_OPS_DMABUF: return sizeof(lpr_dmabuf_backend_t);
    case LPR_FD_OPS_SYNC_FILE: return sizeof(lpr_sync_file_backend_t);
    default: return 0;
    }
}

static int lpr_transaction_track_lease(
    lpr_exec_transaction_t *transaction,
    uint32_t pin_index,
    int fd);

static uint32_t lpr_exec_backend_native_fds(
    uint8_t ops_id,
    const void *state,
    int32_t out_fds[2])
{
    if (out_fds == 0) return 0;
    out_fds[0] = -1;
    out_fds[1] = -1;
    if (state == 0) return 0;
    uint32_t count = 0;
#define LPR_EXEC_ADD_NATIVE(value) do { \
    const int32_t native_fd__ = (value); \
    if (native_fd__ >= 16 && count < 2) out_fds[count++] = native_fd__; \
} while (0)
    switch (ops_id) {
    case LPR_FD_OPS_FILED:
        LPR_EXEC_ADD_NATIVE(((const lpr_filed_backend_t *)state)->lease_fd.raw);
        break;
    case LPR_FD_OPS_TTY:
        LPR_EXEC_ADD_NATIVE(((const lpr_tty_backend_t *)state)->wait_fd.raw);
        LPR_EXEC_ADD_NATIVE(((const lpr_tty_backend_t *)state)->lease_fd.raw);
        break;
    case LPR_FD_OPS_DRM:
        LPR_EXEC_ADD_NATIVE(((const lpr_drm_backend_t *)state)->wait_fd.raw);
        LPR_EXEC_ADD_NATIVE(((const lpr_drm_backend_t *)state)->lease_fd.raw);
        break;
    case LPR_FD_OPS_INPUT:
        LPR_EXEC_ADD_NATIVE(((const lpr_input_backend_t *)state)->wait_fd.raw);
        LPR_EXEC_ADD_NATIVE(((const lpr_input_backend_t *)state)->lease_fd.raw);
        break;
    case LPR_FD_OPS_PIPE:
        LPR_EXEC_ADD_NATIVE(((const lpr_pipe_backend_t *)state)->native.raw);
        break;
    case LPR_FD_OPS_EVENT:
        LPR_EXEC_ADD_NATIVE(((const lpr_event_backend_t *)state)->wait_fd.raw);
        LPR_EXEC_ADD_NATIVE(((const lpr_event_backend_t *)state)->notify_fd.raw);
        break;
    case LPR_FD_OPS_EPOLL:
        LPR_EXEC_ADD_NATIVE(((const lpr_epoll_backend_t *)state)->wait_fd.raw);
        LPR_EXEC_ADD_NATIVE(((const lpr_epoll_backend_t *)state)->notify_fd.raw);
        break;
    case LPR_FD_OPS_SOCKET:
        LPR_EXEC_ADD_NATIVE(((const lpr_socket_backend_t *)state)->wait_fd.raw);
        LPR_EXEC_ADD_NATIVE(((const lpr_socket_backend_t *)state)->lease_fd.raw);
        break;
    case LPR_FD_OPS_DMABUF:
        LPR_EXEC_ADD_NATIVE(((const lpr_dmabuf_backend_t *)state)->native.raw);
        LPR_EXEC_ADD_NATIVE(((const lpr_dmabuf_backend_t *)state)->lease_fd.raw);
        break;
    case LPR_FD_OPS_SYNC_FILE:
        LPR_EXEC_ADD_NATIVE(
            ((const lpr_sync_file_backend_t *)state)->wait_fd.raw);
        break;
    default:
        break;
    }
#undef LPR_EXEC_ADD_NATIVE
    return count;
}

static void lpr_exec_unpin(lpr_exec_transaction_t *transaction)
{
    if (transaction == 0 || transaction->pins == 0) return;
    for (uint64_t i = transaction->pin_count; i != 0; --i) {
        lpr_fd_drop_t drop;
        if (lpr_fd_table_unpin(
                &lpr_control_fd_table, &transaction->pins[i - 1u], &drop) == 0 &&
            drop.ready)
        {
            (void)lpr_backend_finish_drop(&drop);
        }
    }
    transaction->pin_count = 0;
}

static int lpr_backend_needs_transfer_lease(uint8_t ops_id, const void *state)
{
    if (state == 0) return 0;
    switch (ops_id) {
    case LPR_FD_OPS_FILED: {
        const lpr_filed_backend_t *backend = state;
        return backend->handle != 0 &&
            (backend->reserved2 & LPR_BACKEND_TRANSFER_LEASE) == 0;
    }
    case LPR_FD_OPS_TTY: {
        const lpr_tty_backend_t *backend = state;
        return backend->handle != 0 &&
            (backend->reserved1 & LPR_BACKEND_TRANSFER_LEASE) == 0;
    }
    case LPR_FD_OPS_DRM: {
        const lpr_drm_backend_t *backend = state;
        return backend->handle != 0 &&
            (backend->reserved1 & LPR_BACKEND_TRANSFER_LEASE) == 0;
    }
    case LPR_FD_OPS_INPUT: {
        const lpr_input_backend_t *backend = state;
        return backend->handle != 0 &&
            (backend->reserved1 & LPR_BACKEND_TRANSFER_LEASE) == 0;
    }
    case LPR_FD_OPS_SOCKET: {
        const lpr_socket_backend_t *backend = state;
        return backend->handle != 0 &&
            (backend->reserved1 & LPR_BACKEND_TRANSFER_LEASE) == 0;
    }
    case LPR_FD_OPS_DMABUF: {
        const lpr_dmabuf_backend_t *backend = state;
        return backend->token != 0 &&
            (backend->reserved0 & LPR_BACKEND_TRANSFER_LEASE) == 0;
    }
    default:
        return 0;
    }
}

static int lpr_duplicate_manifest_capability(
    lpr_exec_transaction_t *transaction,
    uint32_t pin_index,
    int source_fd,
    int32_t *out_fd)
{
    if (transaction == 0 || out_fd == 0 || source_fd < 16)
        return -LPR_LINUX_EBADF;
    struct pacha_fd_info info;
    lpr_memset(&info, 0, sizeof(info));
    if (!lpr_native_fd_info((uint64_t)(uint32_t)source_fd, &info) ||
        (info.rights & PACHA_FD_RIGHT_DUP) == 0)
        return -LPR_LINUX_EBADF;
    const int64_t duplicate = lpr_pacha_syscall4(
        PACHA_FD_SYSCALL_FCNTL,
        (uint64_t)(uint32_t)source_fd,
        PACHA_FD_FCNTL_DUP,
        16,
        info.rights);
    if (duplicate < 16) return (int)lpr_pacha_status_to_errno(duplicate);
    const int64_t flag_status = lpr_pacha_syscall4(
        PACHA_FD_SYSCALL_FCNTL,
        (uint64_t)(uint32_t)duplicate,
        PACHA_FD_FCNTL_SET_FLAGS,
        0,
        PACHA_FD_FLAG_CLOEXEC);
    if (flag_status != 0) {
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)duplicate);
        return (int)lpr_pacha_status_to_errno(flag_status);
    }
    const int status = lpr_transaction_track_lease(
        transaction, pin_index, (int)duplicate);
    if (status != 0) {
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)duplicate);
        return status;
    }
    *out_fd = (int32_t)duplicate;
    return 0;
}

static void lpr_manifest_diag(const char *stage)
{
    static const char prefix[] = "[lpr] manifest failure stage=";
    char line[96];
    size_t length = 0;
    for (size_t i = 0; i < sizeof(prefix) - 1u && length < sizeof(line); ++i) {
        line[length++] = prefix[i];
    }
    for (size_t i = 0; stage[i] != '\0' && length < sizeof(line) - 1u; ++i) {
        line[length++] = stage[i];
    }
    if (length < sizeof(line)) {
        line[length++] = '\n';
    }
    (void)lpr_pacha_syscall2(
        PACHAOS_SYSCALL_LOG,
        (uint64_t)(uintptr_t)line,
        length);
}

#if defined(LPR_EXEC_DIAG) && LPR_EXEC_DIAG
static char *lpr_manifest_diag_append_text(
    char *out,
    const char *end,
    const char *text)
{
    if (out == 0 || end == 0 || text == 0) return out;
    while (out < end && *text != '\0') *out++ = *text++;
    return out;
}

static char *lpr_manifest_diag_append_i64(
    char *out,
    const char *end,
    int64_t value)
{
    char digits[20];
    uint64_t count = 0;
    uint64_t magnitude = (uint64_t)value;
    if (value < 0) {
        if (out < end) *out++ = '-';
        magnitude = (~magnitude) + 1u;
    }
    do {
        digits[count++] = (char)('0' + magnitude % 10u);
        magnitude /= 10u;
    } while (magnitude != 0 && count < sizeof(digits));
    while (out < end && count != 0) *out++ = digits[--count];
    return out;
}

static void lpr_manifest_filed_diag(
    const char *stage,
    uint32_t pin_index,
    const lpr_filed_backend_t *backend,
    int64_t status)
{
    if (stage == 0 || backend == 0) return;
    char line[512];
    char *out = line;
    const char *end = line + sizeof(line);
    out = lpr_manifest_diag_append_text(
        out, end, "[lpr-fork-filed] native_pid=");
    out = lpr_manifest_diag_append_i64(
        out, end, lpr_pacha_syscall0(PACHAOS_SYSCALL_GETPID));
    out = lpr_manifest_diag_append_text(out, end, " native_tid=");
    out = lpr_manifest_diag_append_i64(
        out, end, lpr_pacha_syscall0(PACHAOS_SYSCALL_GETTID));
    out = lpr_manifest_diag_append_text(out, end, " linux_pid=");
    out = lpr_manifest_diag_append_i64(out, end, lpr_linux_current_pid);
    out = lpr_manifest_diag_append_text(out, end, " stage=");
    out = lpr_manifest_diag_append_text(out, end, stage);
    out = lpr_manifest_diag_append_text(out, end, " pin=");
    out = lpr_manifest_diag_append_i64(out, end, pin_index);
    out = lpr_manifest_diag_append_text(out, end, " handle=");
    out = lpr_manifest_diag_append_i64(out, end, (int64_t)backend->handle);
    out = lpr_manifest_diag_append_text(out, end, " lease_fd=");
    out = lpr_manifest_diag_append_i64(out, end, backend->lease_fd.raw);
    out = lpr_manifest_diag_append_text(out, end, " transfer=");
    out = lpr_manifest_diag_append_i64(
        out,
        end,
        (backend->reserved2 & LPR_BACKEND_TRANSFER_LEASE) != 0);
    out = lpr_manifest_diag_append_text(out, end, " status=");
    out = lpr_manifest_diag_append_i64(out, end, status);
    out = lpr_manifest_diag_append_text(out, end, " path=");
    out = lpr_manifest_diag_append_text(out, end, backend->open_path);
    if (out < end) *out++ = '\n';
    (void)lpr_pacha_syscall2(
        PACHAOS_SYSCALL_LOG,
        (uint64_t)(uintptr_t)line,
        (uint64_t)(out - line));
}
#endif

static const char *lpr_manifest_backend_stage(uint8_t ops_id)
{
    switch (ops_id) {
    case LPR_FD_OPS_FILED: return "backend-filed";
    case LPR_FD_OPS_TTY: return "backend-tty";
    case LPR_FD_OPS_DRM: return "backend-drm";
    case LPR_FD_OPS_INPUT: return "backend-input";
    case LPR_FD_OPS_SOCKET: return "backend-socket";
    case LPR_FD_OPS_DMABUF: return "backend-dmabuf";
    case LPR_FD_OPS_PIPE: return "backend-pipe";
    case LPR_FD_OPS_EVENT: return "backend-event";
    case LPR_FD_OPS_EPOLL: return "backend-epoll";
    case LPR_FD_OPS_SYNC_FILE: return "backend-sync-file";
    default: return "backend-unknown";
    }
}

static int lpr_prepare_backend_record(
    lpr_exec_transaction_t *transaction,
    uint32_t pin_index,
    int fork_snapshot,
    uint8_t ops_id,
    const void *original_state,
    void *prepared_state)
{
    if (transaction == 0 || original_state == 0 || prepared_state == 0)
        return -LPR_LINUX_EFAULT;
    int status = 0;
    if (lpr_backend_needs_transfer_lease(ops_id, original_state)) {
        int lease_fd = -1;
        int remote_lease_fd = -1;
        status = lpr_native_wait_pair(&lease_fd, &remote_lease_fd);
        uint64_t handle = 0;
        if (status == 0) {
            switch (ops_id) {
            case LPR_FD_OPS_FILED:
#if defined(LPR_EXEC_DIAG) && LPR_EXEC_DIAG
                lpr_manifest_filed_diag(
                    "transfer-begin",
                    pin_index,
                    (const lpr_filed_backend_t *)original_state,
                    0);
#endif
                status = (int)lpr_filed_transfer_dup_handle(
                    ((const lpr_filed_backend_t *)original_state)->handle,
                    0,
                    remote_lease_fd,
                    &handle);
#if defined(LPR_EXEC_DIAG) && LPR_EXEC_DIAG
                lpr_manifest_filed_diag(
                    status == 0 ? "transfer-ready" : "transfer-failed",
                    pin_index,
                    (const lpr_filed_backend_t *)original_state,
                    status);
#endif
                if (status == 0 && handle == 0) status = -LPR_LINUX_EIO;
                break;
            case LPR_FD_OPS_TTY:
                status = (int)lpr_termd_transfer_dup_handle(
                    ((const lpr_tty_backend_t *)original_state)->handle,
                    remote_lease_fd,
                    &handle);
                if (status == 0 && handle !=
                    ((const lpr_tty_backend_t *)original_state)->handle)
                    status = -LPR_LINUX_EIO;
                break;
            case LPR_FD_OPS_DRM:
                status = (int)lpr_drm_transfer_dup_handle(
                    ((const lpr_drm_backend_t *)original_state)->handle,
                    remote_lease_fd,
                    &handle);
                if (status == 0 && handle !=
                    ((const lpr_drm_backend_t *)original_state)->handle)
                    status = -LPR_LINUX_EIO;
                break;
            case LPR_FD_OPS_INPUT:
                status = (int)lpr_input_transfer_dup_handle(
                    ((const lpr_input_backend_t *)original_state)->handle,
                    remote_lease_fd,
                    &handle);
                if (status == 0 && handle !=
                    ((const lpr_input_backend_t *)original_state)->handle)
                    status = -LPR_LINUX_EIO;
                break;
            case LPR_FD_OPS_SOCKET:
                status = (int)lpr_netd_transfer_dup_handle(
                    ((const lpr_socket_backend_t *)original_state)->handle,
                    remote_lease_fd);
                break;
            case LPR_FD_OPS_DMABUF:
                status = (int)lpr_drm_prime_transfer_acquire(
                    ((const lpr_dmabuf_backend_t *)original_state)->token,
                    remote_lease_fd);
                break;
            default:
                status = -LPR_LINUX_EINVAL;
                break;
            }
        }
        if (status == 0)
            status = lpr_transaction_track_lease(
                transaction, pin_index, lease_fd);
        if (status != 0) {
            lpr_manifest_diag(lpr_manifest_backend_stage(ops_id));
            if (lease_fd >= 16)
                (void)lpr_close_native_fd_if_open(
                    (uint64_t)(uint32_t)lease_fd);
            if (remote_lease_fd >= 16)
                (void)lpr_close_native_fd_if_open(
                    (uint64_t)(uint32_t)remote_lease_fd);
            return status;
        }
        switch (ops_id) {
        case LPR_FD_OPS_FILED: {
            lpr_filed_backend_t *backend = prepared_state;
            backend->handle = handle;
            backend->lease_fd.raw = lease_fd;
            backend->reserved2 |= LPR_BACKEND_TRANSFER_LEASE;
            break;
        }
        case LPR_FD_OPS_TTY: {
            lpr_tty_backend_t *backend = prepared_state;
            backend->lease_fd.raw = lease_fd;
            backend->reserved1 |= LPR_BACKEND_TRANSFER_LEASE;
            break;
        }
        case LPR_FD_OPS_DRM: {
            lpr_drm_backend_t *backend = prepared_state;
            backend->lease_fd.raw = lease_fd;
            backend->reserved1 |= LPR_BACKEND_TRANSFER_LEASE;
            break;
        }
        case LPR_FD_OPS_INPUT: {
            lpr_input_backend_t *backend = prepared_state;
            backend->lease_fd.raw = lease_fd;
            backend->reserved1 |= LPR_BACKEND_TRANSFER_LEASE;
            break;
        }
        case LPR_FD_OPS_SOCKET: {
            lpr_socket_backend_t *backend = prepared_state;
            backend->lease_fd.raw = lease_fd;
            backend->reserved1 |= LPR_BACKEND_TRANSFER_LEASE;
            break;
        }
        case LPR_FD_OPS_DMABUF: {
            lpr_dmabuf_backend_t *backend = prepared_state;
            backend->lease_fd.raw = lease_fd;
            backend->reserved0 |= LPR_BACKEND_TRANSFER_LEASE;
            break;
        }
        default:
            break;
        }
    }
    if (fork_snapshot) return 0;

#define LPR_DUP_MANIFEST_FIELD(original_fd, prepared_fd) do { \
    if ((original_fd) >= 16) { \
        status = lpr_duplicate_manifest_capability( \
            transaction, pin_index, (original_fd), &(prepared_fd)); \
        if (status != 0) return status; \
    } \
} while (0)
    switch (ops_id) {
    case LPR_FD_OPS_FILED: {
        const lpr_filed_backend_t *original = original_state;
        lpr_filed_backend_t *prepared = prepared_state;
        if ((original->reserved2 & LPR_BACKEND_TRANSFER_LEASE) != 0)
            LPR_DUP_MANIFEST_FIELD(
                original->lease_fd.raw, prepared->lease_fd.raw);
        break;
    }
    case LPR_FD_OPS_SYNC_FILE: {
        const lpr_sync_file_backend_t *original = original_state;
        lpr_sync_file_backend_t *prepared = prepared_state;
        LPR_DUP_MANIFEST_FIELD(
            original->wait_fd.raw, prepared->wait_fd.raw);
        break;
    }
    case LPR_FD_OPS_TTY: {
        const lpr_tty_backend_t *original = original_state;
        lpr_tty_backend_t *prepared = prepared_state;
        LPR_DUP_MANIFEST_FIELD(original->wait_fd.raw, prepared->wait_fd.raw);
        if ((original->reserved1 & LPR_BACKEND_TRANSFER_LEASE) != 0)
            LPR_DUP_MANIFEST_FIELD(
                original->lease_fd.raw, prepared->lease_fd.raw);
        break;
    }
    case LPR_FD_OPS_DRM: {
        const lpr_drm_backend_t *original = original_state;
        lpr_drm_backend_t *prepared = prepared_state;
        LPR_DUP_MANIFEST_FIELD(original->wait_fd.raw, prepared->wait_fd.raw);
        if ((original->reserved1 & LPR_BACKEND_TRANSFER_LEASE) != 0)
            LPR_DUP_MANIFEST_FIELD(
                original->lease_fd.raw, prepared->lease_fd.raw);
        break;
    }
    case LPR_FD_OPS_INPUT: {
        const lpr_input_backend_t *original = original_state;
        lpr_input_backend_t *prepared = prepared_state;
        LPR_DUP_MANIFEST_FIELD(original->wait_fd.raw, prepared->wait_fd.raw);
        if ((original->reserved1 & LPR_BACKEND_TRANSFER_LEASE) != 0)
            LPR_DUP_MANIFEST_FIELD(
                original->lease_fd.raw, prepared->lease_fd.raw);
        break;
    }
    case LPR_FD_OPS_SOCKET: {
        const lpr_socket_backend_t *original = original_state;
        lpr_socket_backend_t *prepared = prepared_state;
        LPR_DUP_MANIFEST_FIELD(original->wait_fd.raw, prepared->wait_fd.raw);
        if ((original->reserved1 & LPR_BACKEND_TRANSFER_LEASE) != 0)
            LPR_DUP_MANIFEST_FIELD(
                original->lease_fd.raw, prepared->lease_fd.raw);
        break;
    }
    case LPR_FD_OPS_DMABUF: {
        const lpr_dmabuf_backend_t *original = original_state;
        lpr_dmabuf_backend_t *prepared = prepared_state;
        LPR_DUP_MANIFEST_FIELD(original->native.raw, prepared->native.raw);
        if ((original->reserved0 & LPR_BACKEND_TRANSFER_LEASE) != 0)
            LPR_DUP_MANIFEST_FIELD(
                original->lease_fd.raw, prepared->lease_fd.raw);
        break;
    }
    case LPR_FD_OPS_PIPE: {
        const lpr_pipe_backend_t *original = original_state;
        lpr_pipe_backend_t *prepared = prepared_state;
        LPR_DUP_MANIFEST_FIELD(original->native.raw, prepared->native.raw);
        break;
    }
    case LPR_FD_OPS_EVENT: {
        const lpr_event_backend_t *original = original_state;
        lpr_event_backend_t *prepared = prepared_state;
        LPR_DUP_MANIFEST_FIELD(original->wait_fd.raw, prepared->wait_fd.raw);
        LPR_DUP_MANIFEST_FIELD(original->notify_fd.raw, prepared->notify_fd.raw);
        break;
    }
    case LPR_FD_OPS_EPOLL: {
        const lpr_epoll_backend_t *original = original_state;
        lpr_epoll_backend_t *prepared = prepared_state;
        LPR_DUP_MANIFEST_FIELD(original->wait_fd.raw, prepared->wait_fd.raw);
        LPR_DUP_MANIFEST_FIELD(original->notify_fd.raw, prepared->notify_fd.raw);
        break;
    }
    default:
        break;
    }
#undef LPR_DUP_MANIFEST_FIELD
    return 0;
}

static int lpr_prepare_manifest_impl(
    filed_exec_path_t *exec,
    lpr_exec_transaction_t *transaction,
    int fork_snapshot)
{
    if (exec == 0 || transaction == 0) return -LPR_LINUX_EFAULT;
    lpr_memset(transaction, 0, sizeof(*transaction));
    transaction->manifest_fd = -1;
    transaction->cwd_lease_fd = -1;
    lpr_fd_arrays_init();
    lpr_cwd_init();
    exec->dir_handle = lpr_cwd_handle;
    if (lpr_cwd_handle != 0) {
        int remote_lease_fd = -1;
        int status = lpr_native_wait_pair(
            &transaction->cwd_lease_fd,
            &remote_lease_fd);
        if (status != 0) {
            lpr_manifest_diag("cwd-wait-pair");
            return status;
        }
        const int64_t dup_status = lpr_filed_transfer_dup_handle(
            lpr_cwd_handle,
            0,
            remote_lease_fd,
            &transaction->cwd_handle);
        if (dup_status != 0 || transaction->cwd_handle == 0) {
            (void)lpr_close_native_fd_if_open(
                (uint64_t)(uint32_t)transaction->cwd_lease_fd);
            (void)lpr_close_native_fd_if_open(
                (uint64_t)(uint32_t)remote_lease_fd);
            transaction->cwd_lease_fd = -1;
            transaction->cwd_handle = 0;
            lpr_manifest_diag("cwd-transfer");
            return dup_status != 0 ? (int)dup_status : -LPR_LINUX_EIO;
        }
    }

    const uint64_t capacity = lpr_fd_table_capacity;
    lpr_manifest_layout_t maximum;
    if (capacity == 0 || capacity > UINT64_MAX / 256u ||
        lpr_manifest_layout(
            capacity, capacity, capacity * 2u + 1u,
            capacity * 256u, &maximum) != 0)
    {
        return -LPR_LINUX_E2BIG;
    }
    const uint64_t scratch_offset = lpr_align_up_4096(maximum.byte_size);
    const uint64_t scratch_bytes_per_fd =
        sizeof(lpr_manifest_entry_t) + sizeof(lpr_fd_pin_t) +
        2u * sizeof(lpr_prepared_lease_t) +
        LPR_EXEC_BACKEND_SNAPSHOT_BYTES;
    if (scratch_offset == 0 || scratch_bytes_per_fd == 0 ||
        capacity > (UINT64_MAX - scratch_offset) /
            scratch_bytes_per_fd)
    {
        return -LPR_LINUX_E2BIG;
    }
    const uint64_t map_bytes = lpr_align_up_4096(
        scratch_offset + capacity * scratch_bytes_per_fd);
    if (map_bytes == 0) return -LPR_LINUX_E2BIG;
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_DUP |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_SET_FLAGS |
        PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    const int64_t manifest_fd = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_VMO_CREATE, map_bytes, rights, 0);
    if (manifest_fd < 16) {
        lpr_manifest_diag("vmo-create");
        return (int)lpr_pacha_status_to_errno(manifest_fd);
    }
    const int64_t mapped = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_MMAP, (uint64_t)(uint32_t)manifest_fd, 0, map_bytes,
        PACHAOS_PROT_READ | PACHAOS_PROT_WRITE, PACHAOS_MMAP_SHARED, 0);
    if (mapped < 4096) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)manifest_fd);
        lpr_manifest_diag("vmo-map");
        return (int)lpr_pacha_status_to_errno(mapped);
    }
    lpr_zero_bytes((void *)(uintptr_t)mapped, map_bytes);
    transaction->manifest_fd = (int)manifest_fd;
    transaction->map_bytes = map_bytes;
    transaction->manifest = (lpr_manifest_t *)(uintptr_t)mapped;
    lpr_manifest_entry_t *snapshot_entries =
        (lpr_manifest_entry_t *)((uintptr_t)mapped + scratch_offset);
    transaction->pins = (lpr_fd_pin_t *)(snapshot_entries + capacity);
    transaction->prepared_leases =
        (lpr_prepared_lease_t *)(transaction->pins + capacity);
    transaction->prepared_lease_capacity = capacity * 2u;
    uint8_t *backend_snapshots = (uint8_t *)(
        transaction->prepared_leases + transaction->prepared_lease_capacity);

    uint64_t entry_count = 0;
    lpr_fd_table_lock(&lpr_control_fd_table);
    const uint64_t snapshot_generation = lpr_control_fd_table.generation;
    for (uint64_t fd = 0; fd < capacity; ++fd) {
        const int preserve = fork_snapshot ?
            lpr_fork_local_fd_preserve_unlocked(fd) :
            lpr_exec_local_fd_preserve_unlocked(fd);
        if (!preserve) continue;
        const lpr_fd_entry_t *entry = &lpr_control_fd_table.entries[fd];
        lpr_ofd_t *ofd = &lpr_control_fd_table.ofds[entry->ofd_index];
        lpr_backend_record_t *backend =
            ofd->backend.index < lpr_control_fd_table.backend_count ?
                &lpr_control_fd_table.backends[ofd->backend.index] : 0;
        uint64_t ofd_index = transaction->pin_count;
        for (uint64_t i = 0; i < transaction->pin_count; ++i) {
            if (transaction->pins[i].ofd_index == entry->ofd_index &&
                transaction->pins[i].ofd_generation == entry->ofd_generation)
            {
                ofd_index = i;
                break;
            }
        }
        if (ofd_index == transaction->pin_count) {
            const uint64_t backend_bytes = backend != 0 ?
                lpr_exec_backend_record_bytes(backend->ops_id) : 0;
            if (backend == 0 || !backend->active ||
                backend->generation != ofd->backend.generation ||
                backend->state == 0 || backend_bytes == 0 ||
                backend_bytes > LPR_EXEC_BACKEND_SNAPSHOT_BYTES ||
                backend->state_bytes < backend_bytes)
            {
                lpr_fd_table_unlock(&lpr_control_fd_table);
                lpr_exec_unpin(transaction);
                lpr_destroy_exec_transaction(transaction);
                return -LPR_LINUX_EBADF;
            }
            ofd->pin_count++;
            lpr_fd_pin_t *pin = &transaction->pins[transaction->pin_count++];
            lpr_memset(pin, 0, sizeof(*pin));
            pin->fd = (uint32_t)fd;
            pin->fd_flags = entry->fd_flags;
            pin->access_mode = ofd->access_mode;
            pin->effective_rights = entry->effective_rights;
            pin->status_flags = ofd->status_flags;
            pin->ofd_index = entry->ofd_index;
            pin->ofd_generation = entry->ofd_generation;
            pin->backend_index = ofd->backend.index;
            pin->backend_generation = ofd->backend.generation;
            pin->ops_id = backend->ops_id;
            pin->offset = ofd->offset;
            pin->state = backend_snapshots +
                ofd_index * LPR_EXEC_BACKEND_SNAPSHOT_BYTES;
            lpr_memcpy(pin->state, backend->state, (size_t)backend_bytes);
        }
        snapshot_entries[entry_count++] = (lpr_manifest_entry_t){
            .fd = (uint32_t)fd,
            .ofd_index = (uint32_t)ofd_index,
            .ofd_generation = entry->ofd_generation,
            .fd_flags = entry->fd_flags,
            .state = LPR_MANIFEST_ENTRY_OPEN,
            .effective_rights = entry->effective_rights,
        };
    }
    lpr_fd_table_unlock(&lpr_control_fd_table);

    uint64_t record_bytes = 0;
    uint64_t capability_count = transaction->cwd_lease_fd >= 16 ? 1u : 0u;
    for (uint64_t i = 0; i < transaction->pin_count; ++i) {
        record_bytes += lpr_exec_backend_record_bytes(transaction->pins[i].ops_id);
        int32_t native_fds[2];
        capability_count += lpr_exec_backend_native_fds(
            transaction->pins[i].ops_id,
            transaction->pins[i].state,
            native_fds);
        capability_count += lpr_backend_needs_transfer_lease(
            transaction->pins[i].ops_id,
            transaction->pins[i].state) ? 1u : 0u;
    }
    lpr_manifest_layout_t layout;
    if (lpr_manifest_layout(entry_count, transaction->pin_count,
            capability_count, record_bytes, &layout) != 0 ||
        lpr_manifest_begin(transaction->manifest, map_bytes, &layout,
            entry_count, transaction->pin_count, capability_count, record_bytes) != 0)
    {
        lpr_exec_unpin(transaction);
        lpr_destroy_exec_transaction(transaction);
        return -LPR_LINUX_E2BIG;
    }
    lpr_manifest_t *manifest = transaction->manifest;
    manifest->transaction_id = lpr_next_request_id(&lpr_request_id);
    manifest->generation = snapshot_generation;
    manifest->linux_pid = (uint64_t)(uint32_t)lpr_linux_current_pid;
    manifest->linux_ppid = (uint64_t)(uint32_t)lpr_linux_current_ppid;
    manifest->linux_sid = (uint64_t)(uint32_t)lpr_linux_current_sid;
    manifest->linux_pgrp = (uint64_t)(uint32_t)lpr_linux_current_pgrp;
    manifest->linux_next_pid = (uint64_t)(uint32_t)lpr_linux_next_pid;
    manifest->signal_mask =
        lpr_linux_signal_mask & ~lpr_linux_unblockable_signal_mask();
    manifest->signal_ignored_mask =
        lpr_linux_ignored_signal_mask() & ~lpr_linux_unblockable_signal_mask();
    manifest->cwd_handle = transaction->cwd_handle;
    manifest->cwd_capability_index = transaction->cwd_lease_fd >= 16 ? 0u : UINT64_MAX;
    if (lpr_supervisor_enabled) {
        lprs_process_state_t state;
        if (lpr_supervisor_get_state(&state) == 0) manifest->owner_generation = state.generation;
        manifest->flags |= LPR_MANIFEST_FLAG_SUPERVISOR;
        manifest->supervisor_token = lpr_supervisor_token;
        manifest->supervisor_endpoint_fd = LPR_SUPERVISOR_ENDPOINT_FD;
    }
    const uint64_t cwd_len = lpr_strnlen(lpr_cwd_path, sizeof(lpr_cwd_path));
    if (cwd_len == 0 || cwd_len >= sizeof(manifest->cwd)) {
        lpr_exec_unpin(transaction);
        lpr_destroy_exec_transaction(transaction);
        return -LPR_LINUX_ENAMETOOLONG;
    }
    lpr_memcpy(manifest->cwd, lpr_cwd_path, (size_t)cwd_len + 1u);
    if (lpr_manifest_checked && lpr_process_manifest.ctty[0] != 0) {
        const uint64_t ctty_len = lpr_strnlen(
            lpr_process_manifest.ctty, sizeof(lpr_process_manifest.ctty));
        if (ctty_len < sizeof(manifest->ctty))
            lpr_memcpy(manifest->ctty, lpr_process_manifest.ctty, (size_t)ctty_len + 1u);
    }
    lpr_memcpy(lpr_manifest_entries(manifest), snapshot_entries,
        (size_t)(entry_count * sizeof(*snapshot_entries)));
    lpr_manifest_ofd_t *ofds = lpr_manifest_ofds(manifest);
    lpr_manifest_capability_t *capabilities = lpr_manifest_capabilities(manifest);
    uint8_t *records = (uint8_t *)manifest + manifest->record_offset;
    uint64_t record_cursor = 0;
    uint64_t capability_cursor = 0;
    if (transaction->cwd_lease_fd >= 16) {
        struct pacha_fd_info info;
        lpr_memset(&info, 0, sizeof(info));
        if (!lpr_native_fd_info(
                (uint64_t)(uint32_t)transaction->cwd_lease_fd, &info))
        {
            lpr_exec_unpin(transaction);
            lpr_destroy_exec_transaction(transaction);
            return -LPR_LINUX_EBADF;
        }
        capabilities[capability_cursor] = (lpr_manifest_capability_t){
            .ordinal = (uint32_t)capability_cursor,
            .flags = LPR_MANIFEST_CAPABILITY_CWD_LEASE,
            .native_fd = (uint64_t)(uint32_t)transaction->cwd_lease_fd,
            .rights = info.rights,
        };
        capability_cursor++;
    }
    for (uint64_t i = 0; i < transaction->pin_count; ++i) {
        const lpr_fd_pin_t *pin = &transaction->pins[i];
        const uint64_t bytes = lpr_exec_backend_record_bytes(pin->ops_id);
        void *record = records + record_cursor;
        lpr_memcpy(record, pin->state, (size_t)bytes);
        const int prepare_status = lpr_prepare_backend_record(
            transaction,
            (uint32_t)i,
            fork_snapshot,
            pin->ops_id,
            pin->state,
            record);
        if (prepare_status != 0) {
            lpr_manifest_diag(lpr_manifest_backend_stage(pin->ops_id));
            lpr_exec_unpin(transaction);
            lpr_destroy_exec_transaction(transaction);
            return prepare_status;
        }
        int32_t native_fds[2];
        const uint32_t native_fd_count =
            lpr_exec_backend_native_fds(pin->ops_id, record, native_fds);
        ofds[i].generation = pin->ofd_generation;
        ofds[i].backend_id = pin->ops_id;
        ofds[i].access_mode = pin->access_mode;
        ofds[i].status_flags = pin->status_flags;
        ofds[i].rights_ceiling = pin->effective_rights;
        ofds[i].offset = pin->offset;
        ofds[i].record_offset = record_cursor;
        ofds[i].record_bytes = (uint32_t)bytes;
        ofds[i].capability_first = (uint32_t)capability_cursor;
        ofds[i].capability_count = (uint16_t)native_fd_count;
        record_cursor += bytes;
        for (uint32_t ni = 0; ni < native_fd_count; ++ni) {
            const int32_t native_fd = native_fds[ni];
            struct pacha_fd_info info;
            lpr_memset(&info, 0, sizeof(info));
            if (!lpr_native_fd_info((uint64_t)(uint32_t)native_fd, &info)) {
                lpr_exec_unpin(transaction);
                lpr_destroy_exec_transaction(transaction);
                return -LPR_LINUX_EBADF;
            }
            capabilities[capability_cursor] = (lpr_manifest_capability_t){
                .ordinal = (uint32_t)capability_cursor,
                .native_fd = (uint64_t)(uint32_t)native_fd,
                .rights = info.rights,
            };
            capability_cursor++;
        }
    }
    if (record_cursor != record_bytes || capability_cursor != capability_count) {
        lpr_manifest_diag(
            record_cursor != record_bytes ? "layout-record" :
            capability_cursor < capability_count ? "layout-capability-short" :
            "layout-capability-long");
        lpr_exec_unpin(transaction);
        lpr_destroy_exec_transaction(transaction);
        return -LPR_LINUX_EIO;
    }
    if (lpr_manifest_seal(manifest, map_bytes) != 0) {
        lpr_manifest_diag("seal");
        lpr_exec_unpin(transaction);
        lpr_destroy_exec_transaction(transaction);
        return -LPR_LINUX_EIO;
    }
    exec->flags |= FILED_EXEC_BOOTSTRAP_FD;
    return 0;
}

static int lpr_prepare_manifest(
    filed_exec_path_t *exec,
    lpr_exec_transaction_t *transaction,
    int fork_snapshot)
{
    /* The image cache is optional, while every capability represented in an
     * exec/fork manifest is semantic state.  Keep the cache quiescent until
     * the transaction ends so concurrent mmap cannot consume the native FD
     * slots needed by transfer leases after the initial purge. */
    lpr_file_image_cache_pause();
    const int status =
        lpr_prepare_manifest_impl(exec, transaction, fork_snapshot);
    if (status != 0) {
        lpr_file_image_cache_resume();
        return status;
    }
    transaction->file_image_cache_paused = 1;
    return 0;
}

int lpr_prepare_exec_manifest(
    filed_exec_path_t *exec,
    lpr_exec_transaction_t *transaction)
{
    return lpr_prepare_manifest(exec, transaction, 0);
}

int lpr_prepare_fork_manifest(
    filed_exec_path_t *exec,
    lpr_exec_transaction_t *transaction)
{
    return lpr_prepare_manifest(exec, transaction, 1);
}

static void *lpr_fork_manifest_record(
    const lpr_exec_transaction_t *transaction,
    uint64_t index)
{
    if (transaction == 0 || transaction->manifest == 0 ||
        index >= transaction->manifest->ofd_count)
        return 0;
    const lpr_manifest_ofd_t *ofds = lpr_manifest_ofds(transaction->manifest);
    const lpr_manifest_ofd_t *ofd = &ofds[index];
    if (ofd->record_offset > transaction->manifest->record_bytes ||
        ofd->record_bytes > transaction->manifest->record_bytes - ofd->record_offset)
        return 0;
    return (uint8_t *)transaction->manifest +
        transaction->manifest->record_offset + ofd->record_offset;
}

static int lpr_transaction_track_lease(
    lpr_exec_transaction_t *transaction,
    uint32_t pin_index,
    int fd)
{
    if (transaction == 0 || fd < 16 || transaction->prepared_leases == 0 ||
        transaction->prepared_lease_count >= transaction->prepared_lease_capacity)
        return -LPR_LINUX_EMFILE;
    transaction->prepared_leases[transaction->prepared_lease_count++] =
        (lpr_prepared_lease_t){ .pin_index = pin_index, .fd = fd };
    return 0;
}

void lpr_fork_transaction_rollback(lpr_exec_transaction_t *transaction)
{
    enum { LPR_FORK_PREPARED = 1u, LPR_FORK_COMMITTED = 2u,
        LPR_FORK_ROLLED_BACK = 3u };
    if (transaction == 0 || transaction->fork_state == LPR_FORK_ROLLED_BACK ||
        transaction->fork_state == LPR_FORK_COMMITTED)
        return;
    for (uint64_t i = transaction->prepared_lease_count; i != 0; --i)
        if (transaction->prepared_leases[i - 1u].fd >= 16)
            (void)lpr_close_native_fd_if_open(
                (uint64_t)(uint32_t)transaction->prepared_leases[i - 1u].fd);
    transaction->prepared_lease_count = 0;
    transaction->fork_prepared_count = 0;
    transaction->fork_state = LPR_FORK_ROLLED_BACK;
    if (transaction->cwd_lease_fd >= 16)
        (void)lpr_close_native_fd_if_open(
            (uint64_t)(uint32_t)transaction->cwd_lease_fd);
    transaction->cwd_lease_fd = -1;
    transaction->cwd_handle = 0;
}

int lpr_fork_transaction_prepare(lpr_exec_transaction_t *transaction)
{
    enum { LPR_FORK_PREPARED = 1u };
    if (transaction != 0 && transaction->fork_state == LPR_FORK_PREPARED &&
        transaction->fork_prepared_count == transaction->pin_count)
        return 0;
    if (transaction == 0 || transaction->manifest == 0 ||
        transaction->pins == 0 || transaction->fork_state != 0)
        return -LPR_LINUX_EINVAL;
    if (transaction->manifest->ofd_count != transaction->pin_count)
        return -LPR_LINUX_EIO;
    for (uint64_t i = 0; i < transaction->pin_count; ++i) {
        const lpr_fd_pin_t *pin = &transaction->pins[i];
        void *prepared_state = lpr_fork_manifest_record(transaction, i);
        if (prepared_state == 0 || pin->state == 0 ||
            lpr_exec_backend_record_bytes(pin->ops_id) == 0)
            return -LPR_LINUX_EIO;
    }
    transaction->fork_state = LPR_FORK_PREPARED;
    transaction->fork_prepared_count = transaction->pin_count;
    return 0;
}

int lpr_fork_transaction_commit_child(lpr_exec_transaction_t *transaction)
{
    enum { LPR_FORK_PREPARED = 1u, LPR_FORK_COMMITTED = 2u };
    if (transaction != 0 && transaction->fork_state == LPR_FORK_COMMITTED)
        return 0;
    if (transaction == 0 || transaction->fork_state != LPR_FORK_PREPARED ||
        transaction->fork_prepared_count != transaction->pin_count)
        return -LPR_LINUX_EINVAL;
    for (uint64_t i = 0; i < transaction->pin_count; ++i) {
        const lpr_fd_pin_t *pin = &transaction->pins[i];
        if (pin->ofd_index >= lpr_control_fd_table.ofd_count) return -LPR_LINUX_EIO;
        const lpr_ofd_t *ofd = &lpr_control_fd_table.ofds[pin->ofd_index];
        if (!ofd->active || ofd->generation != pin->ofd_generation ||
            lpr_ofd_ops_id(ofd) != pin->ops_id ||
            lpr_backend_state_from_ofd(ofd) == 0 ||
            lpr_fork_manifest_record(transaction, i) == 0)
            return -LPR_LINUX_EIO;
    }
    for (uint64_t i = 0; i < transaction->pin_count; ++i) {
        const lpr_fd_pin_t *pin = &transaction->pins[i];
        lpr_ofd_t *ofd = &lpr_control_fd_table.ofds[pin->ofd_index];
        void *state = lpr_backend_state_from_ofd(ofd);
        const void *prepared = lpr_fork_manifest_record(transaction, i);
        lpr_memcpy(
            state,
            prepared,
            (size_t)lpr_exec_backend_record_bytes(pin->ops_id));
    }
    if (transaction->cwd_handle != 0) {
        if (lpr_cwd_lease_fd >= 16)
            (void)lpr_close_native_fd_if_open(
                (uint64_t)(uint32_t)lpr_cwd_lease_fd);
        lpr_cwd_handle = transaction->cwd_handle;
        lpr_cwd_lease_fd = transaction->cwd_lease_fd;
    }
    transaction->cwd_handle = 0;
    transaction->cwd_lease_fd = -1;
    transaction->prepared_lease_count = 0;
    transaction->fork_prepared_count = 0;
    transaction->fork_state = LPR_FORK_COMMITTED;
    return 0;
}

int lpr_fork_transaction_commit_parent(lpr_exec_transaction_t *transaction)
{
    enum { LPR_FORK_PREPARED = 1u, LPR_FORK_COMMITTED = 2u };
    if (transaction != 0 && transaction->fork_state == LPR_FORK_COMMITTED)
        return 0;
    if (transaction == 0 || transaction->fork_state != LPR_FORK_PREPARED ||
        transaction->fork_prepared_count != transaction->pin_count)
        return -LPR_LINUX_EINVAL;
    for (uint64_t i = transaction->prepared_lease_count; i != 0; --i)
        if (transaction->prepared_leases[i - 1u].fd >= 16)
            (void)lpr_close_native_fd_if_open(
                (uint64_t)(uint32_t)transaction->prepared_leases[i - 1u].fd);
    transaction->prepared_lease_count = 0;
    if (transaction->cwd_lease_fd >= 16)
        (void)lpr_close_native_fd_if_open(
            (uint64_t)(uint32_t)transaction->cwd_lease_fd);
    transaction->cwd_handle = 0;
    transaction->cwd_lease_fd = -1;
    transaction->fork_prepared_count = 0;
    transaction->fork_state = LPR_FORK_COMMITTED;
    return 0;
}

void lpr_exec_transaction_commit_self(lpr_exec_transaction_t *transaction)
{
    if (transaction == 0) return;

    /* lpr_close_local_state_before_self_exec() has detached every Linux FD,
     * but preserved OFDs are still pinned by the manifest snapshot.  Release
     * those pins while the old address space is alive so deferred backend
     * closes actually retire their source handles and native capabilities.
     *
     * Prepared leases are the replacement capabilities named by the sealed
     * manifest.  Ownership of those FDs moves to the next LPR image, so this
     * transaction must relinquish them instead of closing them. */
    lpr_exec_unpin(transaction);
    transaction->prepared_lease_count = 0;
    transaction->cwd_handle = 0;
    transaction->cwd_lease_fd = -1;

    if (transaction->manifest != 0 && transaction->map_bytes != 0) {
        (void)lpr_pacha_syscall2(
            PACHAOS_SYSCALL_MUNMAP,
            (uint64_t)(uintptr_t)transaction->manifest,
            transaction->map_bytes);
    }
    if (transaction->manifest_fd >= 16 &&
        transaction->manifest_fd != LPR_BOOTSTRAP_FD)
    {
        (void)lpr_close_native_fd_if_open(
            (uint64_t)(uint32_t)transaction->manifest_fd);
    }
    transaction->manifest_fd = -1;
    transaction->manifest = 0;
    transaction->map_bytes = 0;
    transaction->pins = 0;
    transaction->pin_count = 0;
    transaction->prepared_leases = 0;
    transaction->prepared_lease_capacity = 0;
}

void lpr_destroy_exec_transaction(lpr_exec_transaction_t *transaction)
{
    if (transaction == 0) return;
    lpr_fork_transaction_rollback(transaction);
    for (uint64_t i = transaction->prepared_lease_count; i != 0; --i)
        if (transaction->prepared_leases != 0 &&
            transaction->prepared_leases[i - 1u].fd >= 16)
            (void)lpr_close_native_fd_if_open(
                (uint64_t)(uint32_t)transaction->prepared_leases[i - 1u].fd);
    transaction->prepared_lease_count = 0;
    lpr_exec_unpin(transaction);
    if (transaction->manifest != 0 && transaction->map_bytes != 0) {
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP,
            (uint64_t)(uintptr_t)transaction->manifest, transaction->map_bytes);
    }
    if (transaction->manifest_fd >= 16) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE,
            (uint64_t)(uint32_t)transaction->manifest_fd);
    }
    if (transaction->cwd_lease_fd >= 16)
        (void)lpr_close_native_fd_if_open(
            (uint64_t)(uint32_t)transaction->cwd_lease_fd);
    transaction->cwd_lease_fd = -1;
    transaction->cwd_handle = 0;
    transaction->manifest_fd = -1;
    transaction->map_bytes = 0;
    transaction->manifest = 0;
    transaction->pins = 0;
    if (transaction->file_image_cache_paused) {
        transaction->file_image_cache_paused = 0;
        lpr_file_image_cache_resume();
    }
}

void lpr_close_local_state_before_self_exec(void)
{
    lpr_fd_arrays_init();
    for (uint64_t fd = 0; fd < lpr_fd_table_capacity; fd += 1) {
        if (!lpr_runtime_reserved_fd(fd) && lpr_control_fd_active(fd))
            lpr_control_close_fd(fd);
    }
    if (lpr_cwd_handle != 0) {
        if (lpr_cwd_lease_fd >= 16)
            (void)lpr_close_native_fd_if_open(
                (uint64_t)(uint32_t)lpr_cwd_lease_fd);
        else
            (void)lpr_filed_close_handle(lpr_cwd_handle);
        lpr_cwd_handle = 0;
        lpr_cwd_lease_fd = -1;
    }
    lpr_filed_session_drop();
}

void lpr_linux_prepare_process_exit(uint64_t exit_code)
{
    lpr_trace_process_event("exit_prepare", exit_code, 0, 0);
    lpr_fd_arrays_init();
    for (uint64_t fd = 0; fd < lpr_fd_table_capacity; ++fd)
        if (!lpr_runtime_reserved_fd(fd) && lpr_control_fd_active(fd))
            lpr_control_close_fd(fd);
    if (lpr_cwd_handle != 0) {
        const uint64_t handle = lpr_cwd_handle;
        lpr_cwd_handle = 0;
        if (lpr_cwd_lease_fd >= 16)
            (void)lpr_close_native_fd_if_open(
                (uint64_t)(uint32_t)lpr_cwd_lease_fd);
        else
            (void)lpr_filed_close_handle(handle);
        lpr_cwd_lease_fd = -1;
    }
    lpr_filed_session_drop();
}

static int lpr_restore_exec_bootstrap_fd(
    int backup_fd,
    const struct pacha_fd_info *backup_info)
{
    if (backup_fd < 16 || backup_info == 0) return -LPR_LINUX_EBADF;
    const int64_t restored_fd = lpr_pacha_syscall4(
        PACHA_FD_SYSCALL_FCNTL,
        (uint64_t)(uint32_t)backup_fd,
        PACHA_FD_FCNTL_DUP,
        LPR_BOOTSTRAP_FD,
        backup_info->rights);
    if (restored_fd != LPR_BOOTSTRAP_FD) {
        if (restored_fd >= 16) {
            (void)lpr_pacha_syscall1(
                PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)restored_fd);
        }
        return -LPR_LINUX_EIO;
    }
    const uint64_t flag_mask = PACHA_FD_FLAG_CLOEXEC |
        PACHA_FD_FLAG_NONBLOCK | PACHA_FD_FLAG_INHERIT |
        PACHA_FD_FLAG_PRIVATE;
    const int64_t flag_status = lpr_pacha_syscall4(
        PACHA_FD_SYSCALL_FCNTL,
        LPR_BOOTSTRAP_FD,
        PACHA_FD_FCNTL_SET_FLAGS,
        backup_info->flags,
        flag_mask);
    if (flag_status != 0) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, LPR_BOOTSTRAP_FD);
        return (int)lpr_pacha_status_to_errno(flag_status);
    }
    return 0;
}

int lpr_install_exec_bootstrap_fd(int bootstrap_fd)
{
    if (bootstrap_fd < 16) {
        return -LPR_LINUX_EBADF;
    }
    struct pacha_fd_info info;
    lpr_memset(&info, 0, sizeof(info));
    const int64_t info_status = lpr_pacha_syscall2(
        PACHA_FD_SYSCALL_GET_INFO,
        (uint64_t)(uint32_t)bootstrap_fd,
        (uint64_t)(uintptr_t)&info);
    if (info_status != 0) {
        return (int)lpr_pacha_status_to_errno(info_status);
    }
    const uint64_t flags =
        (info.flags & ~(uint64_t)(PACHA_FD_FLAG_CLOEXEC |
            PACHA_FD_FLAG_INHERIT)) |
        PACHA_FD_FLAG_PRIVATE;
    const uint64_t flag_mask =
        PACHA_FD_FLAG_CLOEXEC |
        PACHA_FD_FLAG_NONBLOCK |
        PACHA_FD_FLAG_INHERIT |
        PACHA_FD_FLAG_PRIVATE;
    if (bootstrap_fd == LPR_BOOTSTRAP_FD) {
        const int64_t flag_status = lpr_pacha_syscall4(
            PACHA_FD_SYSCALL_FCNTL,
            (uint64_t)(uint32_t)bootstrap_fd,
            PACHA_FD_FCNTL_SET_FLAGS,
            flags,
            flag_mask);
        return flag_status == 0 ? 0 : (int)lpr_pacha_status_to_errno(flag_status);
    }
    struct pacha_fd_info backup_info;
    lpr_memset(&backup_info, 0, sizeof(backup_info));
    const int64_t backup_info_status = lpr_pacha_syscall2(
        PACHA_FD_SYSCALL_GET_INFO,
        LPR_BOOTSTRAP_FD,
        (uint64_t)(uintptr_t)&backup_info);
    if (backup_info_status != 0) {
        return (int)lpr_pacha_status_to_errno(backup_info_status);
    }
    const int64_t backup_fd = lpr_pacha_syscall4(
        PACHA_FD_SYSCALL_FCNTL,
        LPR_BOOTSTRAP_FD,
        PACHA_FD_FCNTL_DUP,
        16,
        backup_info.rights);
    if (backup_fd < 16) {
        return (int)lpr_pacha_status_to_errno(backup_fd);
    }
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, LPR_BOOTSTRAP_FD);
    const int64_t dup_fd = lpr_pacha_syscall4(
        PACHA_FD_SYSCALL_FCNTL,
        (uint64_t)(uint32_t)bootstrap_fd,
        PACHA_FD_FCNTL_DUP,
        LPR_BOOTSTRAP_FD,
        info.rights);
    if (dup_fd != LPR_BOOTSTRAP_FD) {
        if (dup_fd >= 16) {
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)dup_fd);
        }
        const int restore_status = lpr_restore_exec_bootstrap_fd(
            (int)(uint32_t)backup_fd, &backup_info);
        (void)lpr_pacha_syscall1(
            PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)backup_fd);
        (void)lpr_pacha_syscall1(
            PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)bootstrap_fd);
        return restore_status != 0 ? restore_status : -LPR_LINUX_EIO;
    }
    const int64_t flag_status = lpr_pacha_syscall4(
        PACHA_FD_SYSCALL_FCNTL,
        LPR_BOOTSTRAP_FD,
        PACHA_FD_FCNTL_SET_FLAGS,
        flags,
        flag_mask);
    if (flag_status != 0) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, LPR_BOOTSTRAP_FD);
        const int restore_status = lpr_restore_exec_bootstrap_fd(
            (int)(uint32_t)backup_fd, &backup_info);
        (void)lpr_pacha_syscall1(
            PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)backup_fd);
        (void)lpr_pacha_syscall1(
            PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)bootstrap_fd);
        return restore_status != 0 ? restore_status :
            (int)lpr_pacha_status_to_errno(flag_status);
    }
    (void)lpr_pacha_syscall1(
        PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)backup_fd);
    (void)lpr_pacha_syscall1(
        PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)bootstrap_fd);
    return 0;
}

int64_t lpr_filed_exec_self(
    filed_exec_path_t *exec,
    const lpr_exec_transaction_t *transaction,
    int *out_process_fd,
    int *out_thread_fd,
    int *out_manifest_fd)
{
    if (exec == 0 || transaction == 0 ||
        transaction->manifest_fd < 16 || transaction->manifest == 0 ||
        out_process_fd == 0 || out_thread_fd == 0 || out_manifest_fd == 0)
    {
        return -LPR_LINUX_EFAULT;
    }
    *out_process_fd = -1;
    *out_thread_fd = -1;
    *out_manifest_fd = -1;

    void *page = 0;
    const int page_fd = lpr_create_standalone_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    lpr_memcpy(page, exec, sizeof(*exec));
    lpr_memmove((uint8_t *)page + PACHA_SERVICE_HEADER_BYTES, page, sizeof(*exec));
    lpr_memset(page, 0, PACHA_SERVICE_HEADER_BYTES);

    struct pacha_ipc_fd request_fds[2];
    lpr_memset(request_fds, 0, sizeof(request_fds));
    request_fds[0].fd = (uint64_t)(uint32_t)page_fd;
    request_fds[0].rights =
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    request_fds[1].fd = (uint64_t)(uint32_t)transaction->manifest_fd;
    /* Filed returns this immutable capability record to the caller.  Preserve
     * only the rights needed for that one relay and for installing fd 245. */
    request_fds[1].rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_DUP |
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_SET_FLAGS |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ;
    const uint64_t request_fd_count = 2;

    const uint64_t request_id = lpr_next_request_id(&lpr_request_id);
    pacha_service_envelope_t *header = (pacha_service_envelope_t *)page;
    header->magic = PACHA_SERVICE_REQUEST_MAGIC;
    header->abi_version = PACHA_SERVICE_ABI_VERSION;
    header->service_id = FILED_SERVICE_ID;
    header->op = FILED_OP_EXEC_SELF;
    header->flags = PACHA_SERVICE_FLAG_PAGE_PAYLOAD;
    header->request_id = request_id;
    header->trace_id = request_id;
    header->payload_size = sizeof(*exec);
    header->fd_count = request_fd_count > 0 ? (uint32_t)(request_fd_count - 1u) : 0u;
    const struct pacha_ipc_msg request = {
        .word0 = PACHA_SERVICE_REQUEST_MAGIC,
        .word1 = 0,
        .word2 = 0,
        .word3 = request_id,
        .fds = request_fds,
        .fd_count = request_fd_count,
    };
    lpr_trace_process_event("exec_self_call", 0, 1, 0);
    const int64_t reply_fd = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_IPC_CALL,
        LPR_FILED_ENDPOINT_FD,
        (uint64_t)(uintptr_t)&request);
    if (reply_fd < 16) {
        lpr_destroy_standalone_wire_page(page_fd, page);
        return lpr_pacha_status_to_errno(reply_fd);
    }

    struct pacha_ipc_fd reply_fds[3];
    struct pacha_ipc_msg reply;
    lpr_memset(reply_fds, 0, sizeof(reply_fds));
    lpr_memset(&reply, 0, sizeof(reply));
    reply.fds = reply_fds;
    reply.fd_capacity = 3;
    const int64_t recv_status = lpr_native_ipc_recv_wait(
        (uint64_t)(uint32_t)reply_fd,
        &reply);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)reply_fd);
    lpr_destroy_standalone_wire_page(page_fd, page);
    if (recv_status != 0) {
        return lpr_pacha_status_to_errno(recv_status);
    }
    if (reply.word0 != PACHA_SERVICE_REPLY_MAGIC || reply.word3 != request_id) {
        return -LPR_LINUX_EIO;
    }
    if ((int64_t)reply.word1 < 0) {
        return (int64_t)reply.word1;
    }
    if (reply.fd_count < 3 ||
        reply_fds[0].fd < 16 ||
        reply_fds[1].fd < 16 ||
        reply_fds[2].fd < 16)
    {
        return -LPR_LINUX_EIO;
    }
    *out_process_fd = (int)(uint32_t)reply_fds[0].fd;
    *out_thread_fd = (int)(uint32_t)reply_fds[1].fd;
    *out_manifest_fd = (int)(uint32_t)reply_fds[2].fd;
    return 0;
}
