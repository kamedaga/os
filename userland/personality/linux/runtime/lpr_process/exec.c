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

int lpr_exec_add_string(filed_v2_exec_path_t *exec, filed_v2_exec_string_ref_t *ref, const char *value)
{
    if (exec == 0 || ref == 0 || value == 0) {
        return -LPR_LINUX_EFAULT;
    }
    const uint64_t length = (uint64_t)lpr_strnlen(value, FILED_V2_EXEC_STRING_BYTES) + 1u;
    if (length == 0 || length > UINT16_MAX) {
        return -LPR_LINUX_E2BIG;
    }
    if (exec->string_bytes + length > FILED_V2_EXEC_STRING_BYTES) {
        return -LPR_LINUX_E2BIG;
    }
    ref->offset = (uint16_t)exec->string_bytes;
    ref->length = (uint16_t)length;
    lpr_memcpy(exec->strings + exec->string_bytes, value, (size_t)length);
    exec->string_bytes += length;
    return 0;
}

int64_t lpr_prepare_exec_cwd(filed_v2_exec_path_t *exec)
{
    if (exec == 0) {
        return -LPR_LINUX_EFAULT;
    }
    lpr_cwd_init();
    exec->dir_handle = lpr_cwd_handle;
    exec->cwd_handle = 0;
    const uint64_t cwd_len = (uint64_t)lpr_strnlen(lpr_cwd_path, sizeof(lpr_cwd_path));
    if (cwd_len == 0 || cwd_len >= LPR_BOOTSTRAP_CWD_BYTES) {
        return -LPR_LINUX_ENAMETOOLONG;
    }
    const int status = lpr_exec_add_string(exec, &exec->cwd, lpr_cwd_path);
    if (status != 0) {
        return status;
    }
    if (lpr_cwd_handle == 0) {
        return 0;
    }
    return lpr_filed_dup_handle(lpr_cwd_handle, 0, &exec->cwd_handle);
}

void lpr_discard_exec_cwd(filed_v2_exec_path_t *exec)
{
    if (exec != 0 && exec->cwd_handle != 0) {
        (void)lpr_filed_close_handle(exec->cwd_handle);
        exec->cwd_handle = 0;
    }
}

int lpr_exec_copy_string_vector(
    filed_v2_exec_path_t *exec,
    filed_v2_exec_string_ref_t *refs,
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

uint64_t lpr_exec_fd_table_capacity_for_count(uint64_t count)
{
    uint64_t capacity = LPR_EXEC_LOCAL_FD_TABLE_INITIAL_FDS;
    while (capacity < count) {
        if (capacity > UINT64_MAX / 2u) {
            return 0;
        }
        capacity *= 2u;
    }
    return capacity;
}

uint64_t lpr_align_up_4096(uint64_t value)
{
    const uint64_t mask = 4096ull - 1ull;
    if (value > UINT64_MAX - mask) {
        return 0;
    }
    return (value + mask) & ~mask;
}

uint64_t lpr_exec_fd_table_bytes_for_capacity(uint64_t capacity)
{
    if (capacity >
        (UINT64_MAX - sizeof(filed_v2_exec_lpr_fd_table_t)) /
            sizeof(filed_v2_exec_lpr_fd_t))
    {
        return 0;
    }
    return lpr_align_up_4096(
        sizeof(filed_v2_exec_lpr_fd_table_t) +
        capacity * sizeof(filed_v2_exec_lpr_fd_t));
}

int lpr_exec_local_fd_active(uint64_t fd)
{
    return lpr_fd_is_filed(fd) ||
        lpr_linux_tty_fd_active(fd) ||
        lpr_pipe_fd_is_active(fd) ||
        lpr_linux_eventfd_active(fd) ||
        lpr_linux_socket_fd_active(fd);
}

static int lpr_exec_local_fd_preserve_unlocked(uint64_t fd)
{
    if (fd >= lpr_control_fd_table.slot_count) {
        return 0;
    }
    const lpr_fd_table_slot_t *slot = &lpr_control_fd_table.slots[fd];
    if (!slot->active || slot->file_index >= lpr_control_fd_table.file_count) {
        return 0;
    }
    const lpr_fd_table_file_t *file = &lpr_control_fd_table.files[slot->file_index];
    return file->active &&
        file->kind != LPR_FD_TABLE_KIND_EMPTY &&
        (slot->fd_flags & LPR_FD_TABLE_FD_CLOEXEC) == 0;
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

static void lpr_write_exec_local_fd_desc_unlocked(
    filed_v2_exec_lpr_fd_t *desc,
    uint64_t fd)
{
    lpr_memset(desc, 0, sizeof(*desc));
    desc->fd = fd;
    const lpr_fd_table_slot_t *slot = &lpr_control_fd_table.slots[fd];
    const lpr_fd_table_file_t *file = &lpr_control_fd_table.files[slot->file_index];
    const uint32_t status_flags = lpr_control_status_flags_to_linux(file->status_flags);
    const uint32_t fd_flags =
        (slot->fd_flags & LPR_FD_TABLE_FD_CLOEXEC) != 0 ? LPR_LINUX_O_CLOEXEC : 0;
    if (file->kind == LPR_FD_TABLE_KIND_FILED) {
        desc->kind = FILED_V2_EXEC_LPR_FD_FILED;
        desc->flags = (file->payload.filed.flags & LPR_LINUX_O_ACCMODE) | status_flags | fd_flags;
        desc->handle = file->payload.filed.handle;
        desc->offset_or_counter = file->offset;
    } else if (file->kind == LPR_FD_TABLE_KIND_TTY) {
        desc->kind = FILED_V2_EXEC_LPR_FD_TTY;
        desc->flags = (file->payload.tty.flags & LPR_LINUX_O_ACCMODE) | status_flags | fd_flags;
        desc->handle = file->payload.tty.handle;
    } else if (file->kind == LPR_FD_TABLE_KIND_PIPE) {
        desc->kind = FILED_V2_EXEC_LPR_FD_PIPE;
        desc->flags = (file->payload.pipe.flags & LPR_LINUX_O_ACCMODE) | status_flags | fd_flags;
        desc->handle = fd;
    } else if (file->kind == LPR_FD_TABLE_KIND_EVENT) {
        desc->kind = FILED_V2_EXEC_LPR_FD_EVENT;
        desc->flags = (file->payload.eventfd.flags & LPR_LINUX_O_ACCMODE) | status_flags | fd_flags;
        desc->offset_or_counter = file->payload.eventfd.counter;
    } else if (file->kind == LPR_FD_TABLE_KIND_SOCKET) {
        desc->kind = FILED_V2_EXEC_LPR_FD_SOCKET;
        desc->flags = (file->payload.socket.flags & LPR_LINUX_O_ACCMODE) | status_flags | fd_flags;
        desc->handle = file->payload.socket.handle;
        desc->offset_or_counter = file->payload.socket.type;
    }
}

void lpr_write_exec_local_fd_desc(filed_v2_exec_lpr_fd_t *desc, uint64_t fd)
{
    lpr_fd_arrays_init();
    lpr_fd_table_lock(&lpr_control_fd_table);
    lpr_write_exec_local_fd_desc_unlocked(desc, fd);
    lpr_fd_table_unlock(&lpr_control_fd_table);
}

int lpr_prepare_exec_local_fds(
    filed_v2_exec_path_t *exec,
    lpr_exec_local_fd_table_t *local_table)
{
    if (exec == 0 || local_table == 0) {
        return -LPR_LINUX_EFAULT;
    }
    local_table->fd = -1;
    local_table->map_bytes = 0;
    local_table->table = 0;
    exec->flags &= ~((uint64_t)FILED_V2_EXEC_LPR_FD_TABLE);
    exec->lpr_fd_table_bytes = 0;

    lpr_fd_arrays_init();
    lpr_fd_table_lock(&lpr_control_fd_table);
    const uint64_t snapshot_generation = lpr_control_fd_table.generation;
    uint64_t count = 0;
    for (uint64_t fd_index = 0; fd_index < lpr_fd_table_capacity; fd_index += 1) {
        if (lpr_exec_local_fd_preserve_unlocked(fd_index)) {
            count++;
        }
    }
    if (count == 0) {
        lpr_fd_table_unlock(&lpr_control_fd_table);
        return 0;
    }

    const uint64_t capacity = lpr_exec_fd_table_capacity_for_count(count);
    if (capacity == 0 ||
        capacity > (UINT64_MAX - sizeof(filed_v2_exec_lpr_fd_table_t)) /
            sizeof(filed_v2_exec_lpr_fd_t))
    {
        lpr_fd_table_unlock(&lpr_control_fd_table);
        return -LPR_LINUX_E2BIG;
    }
    const uint64_t map_bytes = lpr_exec_fd_table_bytes_for_capacity(capacity);
    if (map_bytes == 0) {
        lpr_fd_table_unlock(&lpr_control_fd_table);
        return -LPR_LINUX_E2BIG;
    }
    const uint64_t used_bytes =
        sizeof(filed_v2_exec_lpr_fd_table_t) +
        count * sizeof(filed_v2_exec_lpr_fd_t);

    const uint64_t rights =
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    const int64_t fd = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_VMO_CREATE,
        map_bytes,
        rights,
        0);
    if (fd < 16) {
        lpr_fd_table_unlock(&lpr_control_fd_table);
        return (int)lpr_pacha_status_to_errno(fd);
    }
    const int64_t mapped = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_MMAP,
        (uint64_t)(uint32_t)fd,
        0,
        map_bytes,
        PACHAOS_PROT_READ | PACHAOS_PROT_WRITE,
        PACHAOS_MMAP_SHARED,
        0);
    if (mapped < 4096) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)fd);
        lpr_fd_table_unlock(&lpr_control_fd_table);
        return (int)lpr_pacha_status_to_errno(mapped);
    }

    filed_v2_exec_lpr_fd_table_t *table =
        (filed_v2_exec_lpr_fd_table_t *)(uintptr_t)mapped;
    lpr_zero_bytes(table, map_bytes);
    table->magic = FILED_V2_EXEC_LPR_FD_TABLE_MAGIC;
    table->version = FILED_V2_EXEC_LPR_FD_TABLE_VERSION;
    table->byte_size = used_bytes;
    table->fd_count = count;

    filed_v2_exec_lpr_fd_t *entries =
        (filed_v2_exec_lpr_fd_t *)((uintptr_t)mapped + sizeof(*table));
    uint64_t index = 0;
    for (uint64_t fd_index = 0; fd_index < lpr_fd_table_capacity; fd_index += 1) {
        if (!lpr_exec_local_fd_preserve_unlocked(fd_index)) {
            continue;
        }
        if (index >= count) {
            (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)(uintptr_t)mapped, map_bytes);
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)fd);
            lpr_fd_table_unlock(&lpr_control_fd_table);
            return -LPR_LINUX_EIO;
        }
        lpr_write_exec_local_fd_desc_unlocked(&entries[index], fd_index);
        index++;
    }
    if (index != count || lpr_control_fd_table.generation != snapshot_generation) {
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)(uintptr_t)mapped, map_bytes);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)fd);
        lpr_fd_table_unlock(&lpr_control_fd_table);
        return -LPR_LINUX_EIO;
    }

    local_table->fd = (int)fd;
    local_table->map_bytes = map_bytes;
    local_table->table = table;
    exec->flags |= FILED_V2_EXEC_LPR_FD_TABLE;
    exec->lpr_fd_table_bytes = map_bytes;
    lpr_fd_table_unlock(&lpr_control_fd_table);
    return 0;
}

void lpr_destroy_exec_local_fd_table(lpr_exec_local_fd_table_t *local_table)
{
    if (local_table == 0) {
        return;
    }
    if (local_table->table != 0 && local_table->map_bytes != 0) {
        (void)lpr_pacha_syscall2(
            PACHAOS_SYSCALL_MUNMAP,
            (uint64_t)(uintptr_t)local_table->table,
            local_table->map_bytes);
    }
    if (local_table->fd >= 16) {
        (void)lpr_pacha_syscall1(
            PACHAOS_SYSCALL_FD_CLOSE,
            (uint64_t)(uint32_t)local_table->fd);
    }
    local_table->fd = -1;
    local_table->map_bytes = 0;
    local_table->table = 0;
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
            const uint64_t handle = lpr_fd_tty_payload(fd)->handle;
            const int64_t fd_flags = lpr_control_get_fd_flags(fd);
            lpr_control_close_fd(fd);
            if (refcount <= 1 && fd_flags == LPR_LINUX_FD_CLOEXEC && handle != 0) {
                (void)lpr_termd_call_handle(TERMD_V2_OP_HANDLE_CLOSE, handle, 0);
            }
            continue;
        }
        if (lpr_linux_eventfd_active(fd)) {
            lpr_control_close_fd(fd);
            continue;
        }
        if (lpr_fd_is_filed(fd)) {
            uint32_t refcount = 0;
            (void)lpr_fd_table_get_refcount(&lpr_control_fd_table, (uint32_t)fd, &refcount);
            const uint64_t handle = lpr_fd_filed_payload(fd)->handle;
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
    lpr_fd_arrays_init();
    for (uint32_t index = 0; index < lpr_control_fd_table.file_count; index += 1) {
        lpr_fd_object_t *object = &lpr_control_fd_table.files[index];
        if (!object->active) {
            continue;
        }
        if (object->kind == LPR_FD_TABLE_KIND_FILED &&
            object->payload.filed.handle != 0)
        {
            const uint64_t handle = object->payload.filed.handle;
            object->payload.filed.handle = 0;
            object->backend_id = 0;
            (void)lpr_filed_close_handle(handle);
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
    filed_v2_exec_path_t *exec,
    const lpr_exec_local_fd_table_t *local_table,
    int *out_process_fd,
    int *out_thread_fd,
    int *out_bootstrap_fd)
{
    if (exec == 0 || out_process_fd == 0 || out_thread_fd == 0 || out_bootstrap_fd == 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (((exec->flags & FILED_V2_EXEC_LPR_FD_TABLE) != 0) &&
        (local_table == 0 || local_table->fd < 16 || local_table->table == 0))
    {
        return -LPR_LINUX_EINVAL;
    }
    *out_process_fd = -1;
    *out_thread_fd = -1;
    *out_bootstrap_fd = -1;

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
    uint64_t request_fd_count = 1;
    if ((exec->flags & FILED_V2_EXEC_LPR_FD_TABLE) != 0) {
        request_fds[1].fd = (uint64_t)(uint32_t)local_table->fd;
        request_fds[1].rights =
            PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_MAP_READ;
        request_fd_count = 2;
    }

    const uint64_t request_id = ++lpr_request_id;
    pacha_service_request_header_t *header = (pacha_service_request_header_t *)page;
    header->magic = PACHA_SERVICE_REQUEST_MAGIC;
    header->abi_version = PACHA_SERVICE_ABI_VERSION;
    header->service_id = FILED_V2_SERVICE_ID;
    header->op = FILED_V2_OP_EXEC_SELF;
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
    const int64_t recv_status = lpr_pacha_syscall4(
        PACHAOS_SYSCALL_IPC_RECV_WAIT,
        (uint64_t)(uint32_t)reply_fd,
        (uint64_t)(uintptr_t)&reply,
        UINT64_MAX,
        0);
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
    *out_bootstrap_fd = (int)(uint32_t)reply_fds[2].fd;
    return 0;
}
