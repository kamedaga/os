#include "../lpr_filed_internal.h"

int64_t lpr_linux_wait_process_fd(uint64_t process_fd, uint64_t *out_exit_code)
{
    enum { LPR_PROCESS_WAIT_POLL_TICKS = 50 };
    if (process_fd < 16) {
        return -LPR_LINUX_ECHILD;
    }
    lpr_trace_process_event("wait_begin", process_fd, 0, 0);
    lpr_pacha_process_status_t st;
    uint64_t waits = 0;
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
            lpr_trace_process_event("wait_error", process_fd, waits, errno_status);
            return errno_status;
        }
        waits++;
        if (waits == 1 || waits == 16 || waits == 128) {
            lpr_trace_process_event("wait_pending", process_fd, waits, 0);
        }
        lpr_linux_pump_tty_signals();
        struct pacha_pollfd pollfd;
        lpr_memset(&pollfd, 0, sizeof(pollfd));
        pollfd.fd = (int)(uint32_t)process_fd;
        pollfd.events = PACHA_FD_EVENT_READABLE;
        (void)lpr_pacha_syscall4(
            PACHA_FD_SYSCALL_WAIT_MANY,
            (uint64_t)(uintptr_t)&pollfd,
            1,
            LPR_PROCESS_WAIT_POLL_TICKS,
            0);
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
        lpr_pipe_fd_is_active(fd) ||
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
    case LPR_FD_OPS_DMABUF: return sizeof(lpr_dmabuf_backend_t);
    default: return 0;
    }
}

static uint32_t lpr_exec_backend_native_fds(
    const lpr_fd_pin_t *pin,
    int32_t out_fds[2])
{
    if (out_fds == 0) return 0;
    out_fds[0] = -1;
    out_fds[1] = -1;
    if (pin == 0 || pin->state == 0) return 0;
    uint32_t count = 0;
#define LPR_EXEC_ADD_NATIVE(value) do { \
    const int32_t native_fd__ = (value); \
    if (native_fd__ >= 16 && count < 2) out_fds[count++] = native_fd__; \
} while (0)
    switch (pin->ops_id) {
    case LPR_FD_OPS_FILED:
        LPR_EXEC_ADD_NATIVE(((const lpr_filed_backend_t *)pin->state)->lease_fd.raw);
        break;
    case LPR_FD_OPS_TTY:
        LPR_EXEC_ADD_NATIVE(((const lpr_tty_backend_t *)pin->state)->wait_fd.raw);
        break;
    case LPR_FD_OPS_DRM:
        LPR_EXEC_ADD_NATIVE(((const lpr_drm_backend_t *)pin->state)->wait_fd.raw);
        LPR_EXEC_ADD_NATIVE(((const lpr_drm_backend_t *)pin->state)->lease_fd.raw);
        break;
    case LPR_FD_OPS_INPUT:
        LPR_EXEC_ADD_NATIVE(((const lpr_input_backend_t *)pin->state)->wait_fd.raw);
        LPR_EXEC_ADD_NATIVE(((const lpr_input_backend_t *)pin->state)->lease_fd.raw);
        break;
    case LPR_FD_OPS_PIPE:
        LPR_EXEC_ADD_NATIVE(((const lpr_pipe_backend_t *)pin->state)->native.raw);
        break;
    case LPR_FD_OPS_SOCKET:
        LPR_EXEC_ADD_NATIVE(((const lpr_socket_backend_t *)pin->state)->wait_fd.raw);
        break;
    case LPR_FD_OPS_DMABUF:
        LPR_EXEC_ADD_NATIVE(((const lpr_dmabuf_backend_t *)pin->state)->native.raw);
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

int lpr_prepare_exec_manifest(
    filed_exec_path_t *exec,
    lpr_exec_transaction_t *transaction)
{
    if (exec == 0 || transaction == 0) return -LPR_LINUX_EFAULT;
    lpr_memset(transaction, 0, sizeof(*transaction));
    transaction->manifest_fd = -1;
    lpr_fd_arrays_init();
    lpr_cwd_init();
    exec->dir_handle = lpr_cwd_handle;
    if (lpr_cwd_handle != 0) {
        const int64_t status = lpr_filed_dup_handle(
            lpr_cwd_handle, 0, &transaction->cwd_handle);
        if (status != 0) return (int)status;
    }

    const uint64_t capacity = lpr_fd_table_capacity;
    lpr_manifest_layout_t maximum;
    if (capacity == 0 ||
        lpr_manifest_layout(capacity, capacity, capacity, capacity * 256u, &maximum) != 0)
    {
        return -LPR_LINUX_E2BIG;
    }
    const uint64_t scratch_offset = lpr_align_up_4096(maximum.byte_size);
    if (scratch_offset == 0 ||
        capacity > (UINT64_MAX - scratch_offset) /
            (sizeof(lpr_manifest_entry_t) + sizeof(lpr_fd_pin_t)))
    {
        return -LPR_LINUX_E2BIG;
    }
    const uint64_t map_bytes = lpr_align_up_4096(
        scratch_offset + capacity *
            (sizeof(lpr_manifest_entry_t) + sizeof(lpr_fd_pin_t)));
    if (map_bytes == 0) return -LPR_LINUX_E2BIG;
    const uint64_t rights = PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    const int64_t manifest_fd = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_VMO_CREATE, map_bytes, rights, 0);
    if (manifest_fd < 16) return (int)lpr_pacha_status_to_errno(manifest_fd);
    const int64_t mapped = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_MMAP, (uint64_t)(uint32_t)manifest_fd, 0, map_bytes,
        PACHAOS_PROT_READ | PACHAOS_PROT_WRITE, PACHAOS_MMAP_SHARED, 0);
    if (mapped < 4096) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)manifest_fd);
        return (int)lpr_pacha_status_to_errno(mapped);
    }
    lpr_zero_bytes((void *)(uintptr_t)mapped, map_bytes);
    transaction->manifest_fd = (int)manifest_fd;
    transaction->map_bytes = map_bytes;
    transaction->manifest = (lpr_manifest_t *)(uintptr_t)mapped;
    lpr_manifest_entry_t *snapshot_entries =
        (lpr_manifest_entry_t *)((uintptr_t)mapped + scratch_offset);
    transaction->pins = (lpr_fd_pin_t *)(snapshot_entries + capacity);

    uint64_t entry_count = 0;
    lpr_fd_table_lock(&lpr_control_fd_table);
    const uint64_t snapshot_generation = lpr_control_fd_table.generation;
    for (uint64_t fd = 0; fd < capacity; ++fd) {
        if (!lpr_exec_local_fd_preserve_unlocked(fd)) continue;
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
            if (backend == 0 || !backend->active ||
                backend->generation != ofd->backend.generation ||
                lpr_exec_backend_record_bytes(backend->ops_id) == 0)
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
            pin->state = backend->state;
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
    uint64_t capability_count = 0;
    for (uint64_t i = 0; i < transaction->pin_count; ++i) {
        record_bytes += lpr_exec_backend_record_bytes(transaction->pins[i].ops_id);
        int32_t native_fds[2];
        capability_count += lpr_exec_backend_native_fds(
            &transaction->pins[i], native_fds);
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
    manifest->cwd_handle = transaction->cwd_handle;
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
    for (uint64_t i = 0; i < transaction->pin_count; ++i) {
        const lpr_fd_pin_t *pin = &transaction->pins[i];
        const uint64_t bytes = lpr_exec_backend_record_bytes(pin->ops_id);
        int32_t native_fds[2];
        const uint32_t native_fd_count =
            lpr_exec_backend_native_fds(pin, native_fds);
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
        lpr_memcpy(records + record_cursor, pin->state, (size_t)bytes);
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
    if (lpr_manifest_seal(manifest, map_bytes) != 0) {
        lpr_exec_unpin(transaction);
        lpr_destroy_exec_transaction(transaction);
        return -LPR_LINUX_EIO;
    }
    exec->flags |= FILED_EXEC_BOOTSTRAP_FD;
    return 0;
}

void lpr_destroy_exec_transaction(lpr_exec_transaction_t *transaction)
{
    if (transaction == 0) return;
    lpr_exec_unpin(transaction);
    if (transaction->manifest != 0 && transaction->map_bytes != 0) {
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP,
            (uint64_t)(uintptr_t)transaction->manifest, transaction->map_bytes);
    }
    if (transaction->manifest_fd >= 16) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE,
            (uint64_t)(uint32_t)transaction->manifest_fd);
    }
    if (transaction->cwd_handle != 0) {
        (void)lpr_filed_close_handle(transaction->cwd_handle);
        transaction->cwd_handle = 0;
    }
    transaction->manifest_fd = -1;
    transaction->map_bytes = 0;
    transaction->manifest = 0;
    transaction->pins = 0;
}

void lpr_close_local_state_before_self_exec(void)
{
    lpr_fd_arrays_init();
    for (uint64_t fd = 0; fd < lpr_fd_table_capacity; fd += 1) {
        if (lpr_runtime_reserved_fd(fd)) {
            continue;
        }
        if (lpr_pipe_fd_is_active(fd)) {
            lpr_control_close_fd(fd);
            continue;
        }
        if (lpr_linux_socket_fd_active(fd)) {
            (void)lpr_linux_socket_close(fd);
            continue;
        }
        if (lpr_linux_tty_fd_active(fd)) {
            uint32_t refcount = 0;
            (void)lpr_fd_table_get_refcount(&lpr_control_fd_table, (uint32_t)fd, &refcount);
            const uint64_t handle = lpr_tty_backend(fd)->handle;
            const int64_t fd_flags = lpr_control_get_fd_flags(fd);
            lpr_control_close_fd(fd);
            if (refcount <= 1 && fd_flags == LPR_LINUX_FD_CLOEXEC && handle != 0) {
                (void)lpr_termd_call_handle(TERMD_OP_HANDLE_CLOSE, handle, 0);
            }
            continue;
        }
        if (lpr_linux_input_fd_active(fd)) {
            uint32_t refcount = 0;
            (void)lpr_fd_table_get_refcount(&lpr_control_fd_table, (uint32_t)fd, &refcount);
            const uint64_t handle = lpr_input_backend(fd)->handle;
            const int64_t fd_flags = lpr_control_get_fd_flags(fd);
            lpr_control_close_fd(fd);
            if (refcount <= 1 && fd_flags == LPR_LINUX_FD_CLOEXEC && handle != 0) {
                (void)lpr_input_close_handle(handle);
            }
            continue;
        }
        if (lpr_linux_eventfd_active(fd) || lpr_linux_timerfd_active(fd)) {
            lpr_control_close_fd(fd);
            continue;
        }
        if (lpr_linux_dmabuf_fd_active(fd)) {
            uint32_t refcount = 0;
            (void)lpr_fd_table_get_refcount(&lpr_control_fd_table, (uint32_t)fd, &refcount);
            const uint64_t token = lpr_dmabuf_backend(fd)->token;
            const int64_t fd_flags = lpr_control_get_fd_flags(fd);
            lpr_control_close_fd(fd);
            if (refcount <= 1 && fd_flags == LPR_LINUX_FD_CLOEXEC && token != 0) {
                (void)lpr_drm_prime_ref(DRMD_OP_PRIME_RELEASE, token);
            }
            continue;
        }
        if (lpr_linux_epoll_fd_active(fd)) {
            lpr_control_close_fd(fd);
            continue;
        }
        if (lpr_fd_is_filed(fd)) {
            uint32_t refcount = 0;
            (void)lpr_fd_table_get_refcount(&lpr_control_fd_table, (uint32_t)fd, &refcount);
            const uint64_t handle = lpr_filed_backend(fd)->handle;
            const int64_t fd_flags = lpr_control_get_fd_flags(fd);
            lpr_control_close_fd(fd);
            if (refcount <= 1 && fd_flags == LPR_LINUX_FD_CLOEXEC && handle != 0) {
                (void)lpr_filed_close_handle(handle);
            }
        }
    }
}

void lpr_linux_prepare_process_exit(uint64_t exit_code)
{
    lpr_trace_process_event("exit_prepare", exit_code, 0, 0);
    lpr_socket_prepare_process_exit();
    lpr_fd_arrays_init();
    for (uint32_t index = 0; index < lpr_control_fd_table.ofd_count; index += 1) {
        lpr_ofd_t *object = &lpr_control_fd_table.ofds[index];
        if (!object->active) {
            continue;
        }
        if (lpr_ofd_ops_id(object) == LPR_FD_OPS_FILED &&
            ((lpr_filed_backend_t *)lpr_backend_state_from_ofd(object))->handle != 0)
        {
            const uint64_t handle = ((lpr_filed_backend_t *)lpr_backend_state_from_ofd(object))->handle;
            ((lpr_filed_backend_t *)lpr_backend_state_from_ofd(object))->handle = 0;
            (void)lpr_filed_close_handle(handle);
        } else if (lpr_ofd_ops_id(object) == LPR_FD_OPS_DRM &&
            ((lpr_drm_backend_t *)lpr_backend_state_from_ofd(object))->handle != 0)
        {
            const uint64_t handle = ((lpr_drm_backend_t *)lpr_backend_state_from_ofd(object))->handle;
            const int native_wait_fd = ((lpr_drm_backend_t *)lpr_backend_state_from_ofd(object))->wait_fd.raw;
            ((lpr_drm_backend_t *)lpr_backend_state_from_ofd(object))->handle = 0;
            ((lpr_drm_backend_t *)lpr_backend_state_from_ofd(object))->wait_fd.raw = -1;
            if (native_wait_fd >= 16)
                (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)native_wait_fd);
            (void)lpr_drm_close_handle(handle);
        } else if (lpr_ofd_ops_id(object) == LPR_FD_OPS_INPUT &&
            ((lpr_input_backend_t *)lpr_backend_state_from_ofd(object))->handle != 0)
        {
            const uint64_t handle = ((lpr_input_backend_t *)lpr_backend_state_from_ofd(object))->handle;
            const int native_wait_fd = ((lpr_input_backend_t *)lpr_backend_state_from_ofd(object))->wait_fd.raw;
            ((lpr_input_backend_t *)lpr_backend_state_from_ofd(object))->handle = 0;
            ((lpr_input_backend_t *)lpr_backend_state_from_ofd(object))->wait_fd.raw = -1;
            if (native_wait_fd >= 16)
                (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)native_wait_fd);
            (void)lpr_input_close_handle(handle);
        } else if (lpr_ofd_ops_id(object) == LPR_FD_OPS_DMABUF &&
            ((lpr_dmabuf_backend_t *)lpr_backend_state_from_ofd(object))->token != 0)
        {
            const uint64_t token = ((lpr_dmabuf_backend_t *)lpr_backend_state_from_ofd(object))->token;
            ((lpr_dmabuf_backend_t *)lpr_backend_state_from_ofd(object))->token = 0;
            (void)lpr_drm_prime_ref(DRMD_OP_PRIME_RELEASE, token);
        }
    }
    if (lpr_cwd_handle != 0) {
        const uint64_t handle = lpr_cwd_handle;
        lpr_cwd_handle = 0;
        (void)lpr_filed_close_handle(handle);
    }
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
        (info.flags & ~(uint64_t)PACHA_FD_FLAG_CLOEXEC) |
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
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, LPR_BOOTSTRAP_FD);
    const int64_t dup_fd = lpr_pacha_syscall4(
        PACHA_FD_SYSCALL_FCNTL,
        (uint64_t)(uint32_t)bootstrap_fd,
        PACHA_FD_FCNTL_DUP,
        LPR_BOOTSTRAP_FD,
        info.rights);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)bootstrap_fd);
    if (dup_fd != LPR_BOOTSTRAP_FD) {
        if (dup_fd >= 16) {
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)dup_fd);
        }
        return -LPR_LINUX_EIO;
    }
    const int64_t flag_status = lpr_pacha_syscall4(
        PACHA_FD_SYSCALL_FCNTL,
        LPR_BOOTSTRAP_FD,
        PACHA_FD_FCNTL_SET_FLAGS,
        flags,
        flag_mask);
    return flag_status == 0 ? 0 : (int)lpr_pacha_status_to_errno(flag_status);
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
    request_fds[1].rights = PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ;
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
