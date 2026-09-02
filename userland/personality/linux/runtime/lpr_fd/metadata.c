#include "../lpr_filed_internal.h"

static int64_t lpr_linux_file_vmo_call(
    uint32_t op,
    uint64_t request_flags,
    uint64_t fd,
    uint64_t file_offset,
    uint64_t length,
    uint64_t *out_loaded)
{
    if (out_loaded != 0) {
        *out_loaded = 0;
    }
    if (!lpr_fd_is_filed(fd)) {
        return -LPR_LINUX_EBADF;
    }
    if (length == 0) {
        return -LPR_LINUX_EINVAL;
    }

    void *page = 0;
    const int page_fd = lpr_create_pread_vmo_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }

    filed_file_vmo_request_t *file_vmo =
        (filed_file_vmo_request_t *)((uint8_t *)page + PACHA_SERVICE_HEADER_BYTES);
    lpr_memset(page, 0, FILED_PAGE_BYTES);
    file_vmo->handle = lpr_filed_backend(fd)->handle;
    file_vmo->file_offset = file_offset;
    file_vmo->length = length;
    file_vmo->flags = request_flags;

    struct pacha_ipc_fd request_fd;
    struct pacha_ipc_fd reply_fd_items[1];
    struct pacha_ipc_msg request;
    struct pacha_ipc_msg reply;
    lpr_zero_bytes(&request_fd, sizeof(request_fd));
    lpr_zero_bytes(reply_fd_items, sizeof(reply_fd_items));
    lpr_zero_bytes(&request, sizeof(request));
    lpr_zero_bytes(&reply, sizeof(reply));

    request_fd.fd = (uint64_t)(uint32_t)page_fd;
    request_fd.rights =
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;

    const uint64_t request_id = lpr_next_request_id(&lpr_request_id);
    pacha_service_envelope_t *header = (pacha_service_envelope_t *)page;
    header->magic = PACHA_SERVICE_REQUEST_MAGIC;
    header->abi_version = PACHA_SERVICE_ABI_VERSION;
    header->service_id = FILED_SERVICE_ID;
    header->op = op;
    header->flags = PACHA_SERVICE_FLAG_PAGE_PAYLOAD;
    header->request_id = request_id;
    header->trace_id = request_id;
    header->payload_size = sizeof(*file_vmo);
    request.word0 = PACHA_SERVICE_REQUEST_MAGIC;
    request.word1 = 0;
    request.word3 = request_id;
    request.fds = &request_fd;
    request.fd_count = 1;

    const int64_t call_reply_fd = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_IPC_CALL,
        LPR_FILED_ENDPOINT_FD,
        (uint64_t)(uintptr_t)&request);
    if (call_reply_fd < 16) {
        lpr_destroy_pread_vmo_wire_page(page_fd, page);
        return lpr_pacha_status_to_errno(call_reply_fd);
    }

    reply.fds = reply_fd_items;
    reply.fd_capacity = 1;
    const int64_t recv_status = lpr_native_ipc_recv_wait(
        (uint64_t)(uint32_t)call_reply_fd,
        &reply);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)call_reply_fd);
    if (recv_status != 0) {
        lpr_destroy_pread_vmo_wire_page(page_fd, page);
        return lpr_pacha_status_to_errno(recv_status);
    }
    const pacha_service_envelope_t *reply_header = (const pacha_service_envelope_t *)page;
    if (reply.word0 != PACHA_SERVICE_REPLY_MAGIC ||
        reply.word3 != request_id ||
        reply_header->magic != PACHA_SERVICE_REPLY_MAGIC ||
        reply_header->service_id != FILED_SERVICE_ID ||
        reply_header->op != op ||
        reply_header->request_id != request_id)
    {
        lpr_destroy_pread_vmo_wire_page(page_fd, page);
        return -LPR_LINUX_EIO;
    }
    if (reply_header->status < 0) {
        const int64_t status = reply_header->status;
        lpr_destroy_pread_vmo_wire_page(page_fd, page);
        return status;
    }
    if (reply.fd_count != 1 || reply_fd_items[0].fd < 16) {
        lpr_destroy_pread_vmo_wire_page(page_fd, page);
        return -LPR_LINUX_EIO;
    }
    if (out_loaded != 0) {
        *out_loaded = reply_header->result;
    }
    lpr_destroy_pread_vmo_wire_page(page_fd, page);
    return (int64_t)reply_fd_items[0].fd;
}

int64_t lpr_linux_file_vmo(uint64_t fd, uint64_t file_offset, uint64_t length, uint64_t *out_loaded)
{
    return lpr_linux_file_vmo_call(
        FILED_OP_VFS_FILE_VMO,
        0,
        fd,
        file_offset,
        length,
        out_loaded);
}

int64_t lpr_linux_shared_file_vmo(
    uint64_t fd,
    uint64_t file_offset,
    uint64_t length,
    int writable,
    int executable,
    uint64_t *out_file_size)
{
    return lpr_linux_file_vmo_call(
        FILED_OP_VFS_SHARED_FILE_VMO,
        (writable ? FILED_FILE_VMO_WRITE : 0) |
            (executable ? FILED_FILE_VMO_EXEC : 0),
        fd,
        file_offset,
        length,
        out_file_size);
}

int64_t lpr_backend_write(uint64_t fd, uint64_t buf, uint64_t count)
{
    if (lpr_linux_device_fd_active(fd)) {
        if ((lpr_device_backend(fd)->flags & LPR_LINUX_O_ACCMODE) == LPR_LINUX_O_RDONLY) {
            return -LPR_LINUX_EBADF;
        }
        (void)buf;
        return count <= INT64_MAX ? (int64_t)count : -LPR_LINUX_EINVAL;
    }
    if (lpr_linux_tty_fd_active(fd)) {
        return lpr_tty_io(TERMD_OP_HANDLE_WRITE, fd, buf, count);
    }
    if (lpr_linux_eventfd_active(fd)) {
        if (count < sizeof(uint64_t)) {
            return -LPR_LINUX_EINVAL;
        }
        if (buf == 0) {
            return -LPR_LINUX_EFAULT;
        }
        const uint64_t value = *(const uint64_t *)(uintptr_t)buf;
        if (value == UINT64_MAX) return -LPR_LINUX_EINVAL;
        lpr_event_backend_t *event = lpr_event_backend(fd);
#if defined(LPR_GLYCIN_DIAG) && LPR_GLYCIN_DIAG
        if (__atomic_load_n(&lpr_glycin_diag_armed, __ATOMIC_ACQUIRE) != 0u) {
            lpr_glycin_diag_event(
                "event.write.enter", fd, event->counter,
                event->notify_pending, (int64_t)value);
        }
#endif
        for (;;) {
            uint64_t counter = __atomic_load_n(
                &event->counter, __ATOMIC_ACQUIRE);
            while (counter <= (UINT64_MAX - 1u) - value) {
                if (__atomic_compare_exchange_n(
                        &event->counter,
                        &counter,
                        counter + value,
                        0,
                        __ATOMIC_ACQ_REL,
                        __ATOMIC_ACQUIRE))
                {
                    if (value != 0) lpr_event_backend_notify(event);
#if defined(LPR_GLYCIN_DIAG) && LPR_GLYCIN_DIAG
                    if (__atomic_load_n(
                            &lpr_glycin_diag_armed,
                            __ATOMIC_ACQUIRE) != 0u)
                    {
                        lpr_glycin_diag_event(
                            "event.write.exit", fd,
                            __atomic_load_n(
                                &event->counter, __ATOMIC_ACQUIRE),
                            __atomic_load_n(
                                &event->notify_pending, __ATOMIC_ACQUIRE),
                            (int64_t)value);
                    }
#endif
                    return (int64_t)sizeof(uint64_t);
                }
            }
            if ((event->flags & LPR_LINUX_O_NONBLOCK) != 0)
                return -LPR_LINUX_EAGAIN;
            lpr_wait_graph_t graph;
            lpr_wait_deadline_t deadline;
            lpr_wait_graph_init(&graph);
            int64_t wait_status = lpr_wait_graph_add_fd(
                &graph, fd, 0x0004u);
            if (wait_status == 0)
                wait_status = lpr_wait_deadline_init(&deadline, -1);
            if (wait_status == 0)
                wait_status = lpr_wait_graph_block(&graph, &deadline);
            if (wait_status != 0) return wait_status;
        }
    }
    if (lpr_pipe_fd_is_active(fd)) {
        lpr_pipe_backend_t *pipe = lpr_pipe_backend(fd);
        if (pipe == 0 || !pipe->writable || pipe->native.raw < 0) {
            return -LPR_LINUX_EBADF;
        }
        if (count == 0) {
            return 0;
        }
        if (buf == 0) {
            return -LPR_LINUX_EFAULT;
        }
        for (;;) {
            const int64_t n = lpr_pacha_syscall3(
                PACHAOS_SYSCALL_FD_WRITE,
                (uint64_t)(uint32_t)pipe->native.raw,
                buf,
                count);
            if (n >= 0) return n;
            const int64_t err = lpr_pacha_status_to_errno(n);
            if (err == -LPR_LINUX_EPIPE) {
                lpr_linux_raise_sigpipe();
                return err;
            }
            if (err != -LPR_LINUX_EAGAIN ||
                (pipe->flags & LPR_LINUX_O_NONBLOCK) != 0)
            {
                return err;
            }
            const uint64_t min_write = count <= LPR_LINUX_PIPE_BUF_BYTES ? count : 1;
            const int64_t wait_status = lpr_pipe_wait(fd, 0x0004u, min_write);
            if (wait_status != 0) {
                return wait_status;
            }
        }
    }
    if (lpr_fd_is_filed(fd)) {
        const lpr_filed_backend_t *file = lpr_filed_backend(fd);
        if (lpr_memfd_write_is_sealed(file->reserved1)) {
            return -LPR_LINUX_EPERM;
        }
        if (count != 0) {
            lpr_page_cache_invalidate_handle(file->handle);
        }
        return lpr_filed_io(FILED_OP_VFS_WRITE, fd, buf, count, 0);
    }
    return -LPR_LINUX_EBADF;
}

int64_t lpr_backend_writev(uint64_t fd, uint64_t iov_raw, uint64_t iov_count)
{
    if (iov_raw == 0 && iov_count != 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (lpr_linux_device_fd_active(fd)) {
        if ((lpr_device_backend(fd)->flags & LPR_LINUX_O_ACCMODE) == LPR_LINUX_O_RDONLY) {
            return -LPR_LINUX_EBADF;
        }
        const lpr_linux_iovec_t *iov = (const lpr_linux_iovec_t *)(uintptr_t)iov_raw;
        uint64_t total = 0;
        for (uint64_t i = 0; i < iov_count; i += 1) {
            if (iov[i].len > INT64_MAX - total) return -LPR_LINUX_EINVAL;
            total += iov[i].len;
        }
        return (int64_t)total;
    }
    if (lpr_linux_tty_fd_active(fd)) {
        return lpr_iov_scalar_io(fd, iov_raw, iov_count, 1);
    }
    if (!lpr_fd_is_filed(fd)) {
        if (lpr_pipe_fd_is_active(fd)) {
            lpr_pipe_backend_t *pipe = lpr_pipe_backend(fd);
            if (pipe == 0 || !pipe->writable || pipe->native.raw < 0) {
                return -LPR_LINUX_EBADF;
            }
            for (;;) {
                const int64_t n = lpr_pacha_syscall3(
                    PACHAOS_SYSCALL_FD_WRITEV,
                    (uint64_t)(uint32_t)pipe->native.raw,
                    iov_raw,
                    iov_count);
                if (n >= 0) {
                    return n;
                }
                const int64_t err = lpr_pacha_status_to_errno(n);
                if (err == -LPR_LINUX_EPIPE) {
                    lpr_linux_raise_sigpipe();
                    return err;
                }
                if (err != -LPR_LINUX_EAGAIN ||
                    (pipe->flags & LPR_LINUX_O_NONBLOCK) != 0)
                {
                    return err;
                }
                const uint64_t min_write = lpr_pipe_writev_wait_min(iov_raw, iov_count);
                const int64_t wait_status = lpr_pipe_wait(fd, 0x0004u, min_write);
                if (wait_status != 0) {
                    return wait_status;
                }
            }
        }
        if (lpr_linux_eventfd_active(fd)) {
            int64_t total = 0;
            const lpr_linux_iovec_t *iov = (const lpr_linux_iovec_t *)(uintptr_t)iov_raw;
            for (uint64_t i = 0; i < iov_count; i += 1) {
                const int64_t n = lpr_linux_write(fd, iov[i].base, iov[i].len);
                if (n < 0) {
                    return total != 0 ? total : n;
                }
                total += n;
                if ((uint64_t)n < iov[i].len) {
                    break;
                }
            }
            return total;
        }
        return -LPR_LINUX_EBADF;
    }
    const lpr_linux_iovec_t *iov = (const lpr_linux_iovec_t *)(uintptr_t)iov_raw;
    lpr_page_cache_invalidate_handle(lpr_filed_backend(fd)->handle);
    int64_t total = 0;
    for (uint64_t i = 0; i < iov_count; i += 1) {
        if (iov[i].len == 0) {
            continue;
        }
        const int64_t n = lpr_linux_write(fd, iov[i].base, iov[i].len);
        if (n < 0) {
            return total != 0 ? total : n;
        }
        total += n;
        if ((uint64_t)n < iov[i].len) {
            break;
        }
    }
    return total;
}

int64_t lpr_linux_close(uint64_t fd)
{
    if (fd > LPR_LINUX_FD_MAX || !lpr_control_fd_active(fd)) {
        return -LPR_LINUX_EBADF;
    }
    lpr_epoll_before_close(fd);
    lpr_fd_drop_t drop;
    if (lpr_fd_table_close(&lpr_control_fd_table, (uint32_t)fd, &drop) != 0) {
        return -LPR_LINUX_EBADF;
    }
    return drop.ready ? lpr_backend_finish_drop(&drop) : 0;
}

int64_t lpr_linux_close_range(uint64_t first, uint64_t last, uint64_t flags)
{
    const uint64_t known_flags =
        LPR_LINUX_CLOSE_RANGE_UNSHARE |
        LPR_LINUX_CLOSE_RANGE_CLOEXEC;
    if (first > last || (flags & ~known_flags) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    lpr_fd_arrays_init();
    const int cloexec = (flags & LPR_LINUX_CLOSE_RANGE_CLOEXEC) != 0;
    uint64_t local_last = last;
    if (local_last >= lpr_fd_table_capacity) {
        local_last = lpr_fd_table_capacity == 0 ? 0 : lpr_fd_table_capacity - 1u;
    }
    if (first <= local_last) {
        for (uint64_t fd = first; fd <= local_last; fd += 1) {
            if (lpr_runtime_reserved_fd(fd)) {
                continue;
            }
            if (cloexec) {
                if (lpr_control_fd_active(fd)) {
                    (void)lpr_control_set_fd_flags(fd, LPR_LINUX_FD_CLOEXEC);
                }
                continue;
            }
            if (lpr_control_fd_active(fd)) {
                (void)lpr_linux_close(fd);
            }
        }
    }
    return 0;
}

int64_t lpr_linux_lseek(uint64_t fd, uint64_t offset, uint64_t whence)
{
    if (fd > LPR_LINUX_FD_MAX) {
        return -LPR_LINUX_ESPIPE;
    }
    lpr_fd_pin_t pin;
    if (lpr_fd_table_pin(&lpr_control_fd_table, (uint32_t)fd, &pin) != 0) {
        return -LPR_LINUX_ESPIPE;
    }

    int64_t result = -LPR_LINUX_ESPIPE;
    if (pin.ops_id == LPR_FD_OPS_DEVICE) {
        result = whence <= 2 ? 0 : -LPR_LINUX_EINVAL;
        goto out;
    }
    if (pin.ops_id == LPR_FD_OPS_DMABUF) {
        const lpr_dmabuf_backend_t *dmabuf =
            (const lpr_dmabuf_backend_t *)pin.state;
        if ((int64_t)offset != 0 || dmabuf == 0) {
            result = -LPR_LINUX_EINVAL;
        } else if (whence == 0) {
            result = 0;
        } else if (whence == 2 && dmabuf->size <= INT64_MAX) {
            result = (int64_t)dmabuf->size;
        } else {
            result = -LPR_LINUX_EINVAL;
        }
        goto out;
    }
    if (pin.ops_id != LPR_FD_OPS_FILED || pin.state == 0) {
        goto out;
    }

    lpr_filed_backend_t *filed = (lpr_filed_backend_t *)pin.state;
    if ((filed->flags & LPR_LINUX_O_ACCMODE) == LPR_LINUX_O_RDONLY &&
        filed->offset_valid && whence <= 1)
    {
        const int64_t signed_offset = (int64_t)offset;
        uint64_t new_offset = 0;
        if (lpr_fd_table_seek_pinned(
                &lpr_control_fd_table,
                &pin,
                signed_offset,
                (uint32_t)whence,
                &new_offset) != 0)
        {
            result = -LPR_LINUX_EINVAL;
            goto out;
        }
        filed->pread_active = 1;
        result = (int64_t)new_offset;
        goto out;
    }
    void *page = 0;
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        result = page_fd;
        goto out;
    }
    filed_seek_t *seek = (filed_seek_t *)page;
    lpr_memset(seek, 0, sizeof(*seek));
    seek->handle = filed->handle;
    seek->offset = (int64_t)offset;
    seek->whence = whence;
    uint64_t new_offset = 0;
    const int64_t status =
        lpr_filed_call(FILED_OP_VFS_SEEK, page_fd, 0, &new_offset);
    lpr_destroy_wire_page(page_fd, page);
    if (status == 0 &&
        (filed->flags & LPR_LINUX_O_ACCMODE) == LPR_LINUX_O_RDONLY)
    {
        lpr_filed_control_set_offset(fd, new_offset);
        filed->offset_valid = 1;
        filed->pread_active = 1;
    }
    result = status == 0 ? (int64_t)new_offset : status;

out:
    lpr_fd_unpin(&pin);
    return result;
}

int64_t lpr_linux_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg)
{
    if (lpr_linux_sync_file_fd_active(fd)) {
        const lpr_sync_file_backend_t *sync_file =
            lpr_sync_file_backend(fd);
        switch (cmd) {
        case LPR_LINUX_F_GETFD:
            return lpr_control_get_fd_flags(fd);
        case LPR_LINUX_F_SETFD:
            return lpr_control_set_fd_flags(fd, arg);
        case LPR_LINUX_F_GETFL:
            return lpr_control_get_status_flags(
                fd, sync_file->flags & LPR_LINUX_O_ACCMODE);
        case LPR_LINUX_F_SETFL:
            return lpr_control_set_status_flags(fd, arg);
        case LPR_LINUX_F_DUPFD:
        case LPR_LINUX_F_DUPFD_CLOEXEC:
            return lpr_linux_dup(fd, arg, cmd == LPR_LINUX_F_DUPFD_CLOEXEC);
        default:
            return -LPR_LINUX_EINVAL;
        }
    }
    if (lpr_linux_device_fd_active(fd)) {
        switch (cmd) {
        case LPR_LINUX_F_GETFD: return lpr_control_get_fd_flags(fd);
        case LPR_LINUX_F_SETFD: return lpr_control_set_fd_flags(fd, arg);
        case LPR_LINUX_F_GETFL:
            return lpr_control_get_status_flags(
                fd, lpr_device_backend(fd)->flags & LPR_LINUX_O_ACCMODE);
        case LPR_LINUX_F_SETFL: return lpr_control_set_status_flags(fd, arg);
        case LPR_LINUX_F_DUPFD:
        case LPR_LINUX_F_DUPFD_CLOEXEC:
            return lpr_linux_dup(fd, arg, cmd == LPR_LINUX_F_DUPFD_CLOEXEC);
        default: return -LPR_LINUX_EINVAL;
        }
    }
    if (lpr_linux_epoll_fd_active(fd)) {
        switch (cmd) {
        case LPR_LINUX_F_GETFD:
            return lpr_control_get_fd_flags(fd);
        case LPR_LINUX_F_SETFD:
            return lpr_control_set_fd_flags(fd, arg);
        case LPR_LINUX_F_GETFL:
            return lpr_control_get_status_flags(fd, LPR_LINUX_O_RDWR);
        case LPR_LINUX_F_SETFL:
            return lpr_control_set_status_flags(fd, arg);
        case LPR_LINUX_F_DUPFD:
        case LPR_LINUX_F_DUPFD_CLOEXEC:
            return lpr_linux_dup(fd, arg, cmd == LPR_LINUX_F_DUPFD_CLOEXEC);
        default:
            return -LPR_LINUX_EINVAL;
        }
    }
    if (lpr_linux_tty_fd_active(fd)) {
        switch (cmd) {
        case LPR_LINUX_F_GETFD:
            return lpr_control_get_fd_flags(fd);
        case LPR_LINUX_F_SETFD:
            return lpr_control_set_fd_flags(fd, arg);
        case LPR_LINUX_F_GETFL:
            return lpr_control_get_status_flags(fd, lpr_tty_backend(fd)->flags & LPR_LINUX_O_ACCMODE);
        case LPR_LINUX_F_SETFL:
            return lpr_control_set_status_flags(fd, arg);
        case LPR_LINUX_F_DUPFD:
        case LPR_LINUX_F_DUPFD_CLOEXEC:
            return lpr_linux_dup(fd, arg, cmd == LPR_LINUX_F_DUPFD_CLOEXEC);
        default:
            return -LPR_LINUX_EINVAL;
        }
    }
    if (lpr_linux_drm_fd_active(fd)) {
        switch (cmd) {
        case LPR_LINUX_F_GETFD:
            return lpr_control_get_fd_flags(fd);
        case LPR_LINUX_F_SETFD:
            return lpr_control_set_fd_flags(fd, arg);
        case LPR_LINUX_F_GETFL:
            return lpr_control_get_status_flags(fd, lpr_drm_backend(fd)->flags & LPR_LINUX_O_ACCMODE);
        case LPR_LINUX_F_SETFL:
            return lpr_control_set_status_flags(fd, arg);
        case LPR_LINUX_F_DUPFD:
        case LPR_LINUX_F_DUPFD_CLOEXEC:
            return lpr_linux_dup(fd, arg, cmd == LPR_LINUX_F_DUPFD_CLOEXEC);
        default:
            return -LPR_LINUX_EINVAL;
        }
    }
    if (lpr_linux_input_fd_active(fd)) {
        switch (cmd) {
        case LPR_LINUX_F_GETFD:
            return lpr_control_get_fd_flags(fd);
        case LPR_LINUX_F_SETFD:
            return lpr_control_set_fd_flags(fd, arg);
        case LPR_LINUX_F_GETFL:
            return lpr_control_get_status_flags(
                fd, lpr_input_backend(fd)->flags & LPR_LINUX_O_ACCMODE);
        case LPR_LINUX_F_SETFL:
            return lpr_control_set_status_flags(fd, arg);
        case LPR_LINUX_F_DUPFD:
        case LPR_LINUX_F_DUPFD_CLOEXEC:
            return lpr_linux_dup(fd, arg, cmd == LPR_LINUX_F_DUPFD_CLOEXEC);
        default:
            return -LPR_LINUX_EINVAL;
        }
    }
    if (lpr_linux_dmabuf_fd_active(fd)) {
        lpr_dmabuf_backend_t *dmabuf = lpr_dmabuf_backend(fd);
        switch (cmd) {
        case LPR_LINUX_F_GETFD:
            return lpr_control_get_fd_flags(fd);
        case LPR_LINUX_F_SETFD:
            return lpr_control_set_fd_flags(fd, arg);
        case LPR_LINUX_F_GETFL:
            return lpr_control_get_status_flags(
                fd, dmabuf->writable ? LPR_LINUX_O_RDWR : LPR_LINUX_O_RDONLY);
        case LPR_LINUX_F_SETFL:
            return lpr_control_set_status_flags(fd, arg);
        case LPR_LINUX_F_DUPFD:
        case LPR_LINUX_F_DUPFD_CLOEXEC:
            return lpr_linux_dup(fd, arg, cmd == LPR_LINUX_F_DUPFD_CLOEXEC);
        default:
            return -LPR_LINUX_EINVAL;
        }
    }
    if (lpr_linux_eventfd_active(fd) || lpr_linux_inotify_active(fd) ||
        lpr_linux_timerfd_active(fd) ||
        lpr_linux_signalfd_active(fd)) {
        switch (cmd) {
        case LPR_LINUX_F_GETFD:
            return lpr_control_get_fd_flags(fd);
        case LPR_LINUX_F_SETFD:
            return lpr_control_set_fd_flags(fd, arg);
        case LPR_LINUX_F_GETFL:
            return lpr_control_get_status_flags(fd, lpr_event_backend(fd)->flags & LPR_LINUX_O_ACCMODE);
        case LPR_LINUX_F_SETFL:
            return lpr_control_set_status_flags(fd, arg);
        case LPR_LINUX_F_DUPFD:
        case LPR_LINUX_F_DUPFD_CLOEXEC:
            return lpr_linux_dup(fd, arg, cmd == LPR_LINUX_F_DUPFD_CLOEXEC);
        default:
            return -LPR_LINUX_EINVAL;
        }
    }
    if (lpr_pipe_fd_is_active(fd)) {
        switch (cmd) {
        case LPR_LINUX_F_GETFD:
            return lpr_control_get_fd_flags(fd);
        case LPR_LINUX_F_SETFD:
            return lpr_control_set_fd_flags(fd, arg);
        case LPR_LINUX_F_GETFL:
            return lpr_control_get_status_flags(fd, lpr_pipe_backend(fd)->flags & LPR_LINUX_O_ACCMODE);
        case LPR_LINUX_F_SETFL: {
            const uint64_t pacha_flags = (arg & LPR_LINUX_O_NONBLOCK) != 0 ? PACHA_FD_FLAG_NONBLOCK : 0;
            const int64_t status = lpr_pacha_syscall3(
                PACHAOS_SYSCALL_FD_SET_FLAGS,
                (uint64_t)(uint32_t)lpr_pipe_backend(fd)->native.raw,
                pacha_flags,
                PACHA_FD_FLAG_NONBLOCK);
            if (status != 0) {
                return lpr_pacha_status_to_errno(status);
            }
            return lpr_control_set_status_flags(fd, arg);
        }
        case LPR_LINUX_F_DUPFD:
        case LPR_LINUX_F_DUPFD_CLOEXEC:
            return lpr_linux_dup(fd, arg, cmd == LPR_LINUX_F_DUPFD_CLOEXEC);
        default:
            return -LPR_LINUX_EINVAL;
        }
    }
    if (!lpr_fd_is_filed(fd)) {
        return -LPR_LINUX_EBADF;
    }
    switch (cmd) {
    case LPR_LINUX_F_GETFD:
        return lpr_control_get_fd_flags(fd);
    case LPR_LINUX_F_SETFD:
        return lpr_control_set_fd_flags(fd, arg);
    case LPR_LINUX_F_GETFL:
        return lpr_control_get_status_flags(fd, lpr_filed_backend(fd)->flags & LPR_LINUX_O_ACCMODE);
    case LPR_LINUX_F_SETFL:
        return lpr_control_set_status_flags(fd, arg);
    case LPR_LINUX_F_GETLK:
        if (arg == 0) {
            return -LPR_LINUX_EFAULT;
        }
        *(int16_t *)(uintptr_t)arg = LPR_LINUX_F_UNLCK;
        return 0;
    case LPR_LINUX_F_SETLK:
    case LPR_LINUX_F_SETLKW:
        return arg != 0 ? 0 : -LPR_LINUX_EFAULT;
    case LPR_LINUX_F_ADD_SEALS: {
        lpr_filed_backend_t *file = lpr_filed_backend(fd);
        return lpr_memfd_add_seals(&file->reserved1, arg);
    }
    case LPR_LINUX_F_GET_SEALS: {
        const lpr_filed_backend_t *file = lpr_filed_backend(fd);
        return (file->reserved1 & LPR_FILED_FD_MEMFD) != 0 ?
            (int64_t)(file->reserved1 & LPR_FILED_FD_SEALS) : -LPR_LINUX_EINVAL;
    }
    case LPR_LINUX_F_DUPFD:
    case LPR_LINUX_F_DUPFD_CLOEXEC:
        return lpr_linux_dup(fd, arg, cmd == LPR_LINUX_F_DUPFD_CLOEXEC);
    default:
        return -LPR_LINUX_EINVAL;
    }
}

int64_t lpr_linux_flock(uint64_t fd, uint64_t operation)
{
    if (!lpr_fd_is_filed(fd) && !lpr_linux_eventfd_active(fd) && !lpr_linux_tty_fd_active(fd)) {
        return -LPR_LINUX_EBADF;
    }
    const uint64_t lock_op = operation & ~(uint64_t)LPR_LINUX_LOCK_NB;
    switch (lock_op) {
    case LPR_LINUX_LOCK_SH:
    case LPR_LINUX_LOCK_EX:
    case LPR_LINUX_LOCK_UN:
        return 0;
    default:
        return -LPR_LINUX_EINVAL;
    }
}

int64_t lpr_backend_ioctl(uint64_t fd, uint64_t request, uint64_t arg)
{
    if (request == LPR_LINUX_FIONBIO) {
        if (arg == 0) {
            return -LPR_LINUX_EFAULT;
        }
        const int64_t current = lpr_linux_fcntl(fd, LPR_LINUX_F_GETFL, 0);
        if (current < 0) {
            return current;
        }
        const int enabled = *(const int *)(uintptr_t)arg != 0;
        const uint64_t flags = enabled
            ? (uint64_t)current | LPR_LINUX_O_NONBLOCK
            : (uint64_t)current & ~LPR_LINUX_O_NONBLOCK;
        return lpr_linux_fcntl(fd, LPR_LINUX_F_SETFL, flags);
    }
    if (lpr_linux_dmabuf_fd_active(fd)) {
        return lpr_dmabuf_ioctl(fd, request, arg);
    }
    if (lpr_linux_input_fd_active(fd)) {
        return lpr_input_ioctl(fd, request, arg);
    }
    if (lpr_linux_drm_fd_active(fd)) {
        return lpr_drm_ioctl(fd, request, arg);
    }
    if (lpr_linux_tty_fd_active(fd)) {
        return lpr_tty_ioctl(fd, request, arg);
    }
    switch (request) {
    case LPR_LINUX_TCGETS:
    case LPR_LINUX_TIOCGWINSZ:
        return -LPR_LINUX_ENOTTY;
    default:
        return -LPR_LINUX_ENOTTY;
    }
}

uint8_t lpr_dtype_from_mode(uint64_t mode)
{
    switch (mode & LPR_LINUX_S_IFMT) {
    case LPR_LINUX_S_IFIFO:
        return LPR_LINUX_DT_FIFO;
    case LPR_LINUX_S_IFCHR:
        return LPR_LINUX_DT_CHR;
    case LPR_LINUX_S_IFDIR:
        return LPR_LINUX_DT_DIR;
    case LPR_LINUX_S_IFBLK:
        return LPR_LINUX_DT_BLK;
    case LPR_LINUX_S_IFREG:
        return LPR_LINUX_DT_REG;
    case LPR_LINUX_S_IFLNK:
        return LPR_LINUX_DT_LNK;
    case LPR_LINUX_S_IFSOCK:
        return LPR_LINUX_DT_SOCK;
    default:
        return LPR_LINUX_DT_UNKNOWN;
    }
}

void lpr_write_linux_stat(void *statbuf, const filed_statx_t *wire)
{
    lpr_linux_stat_t *st = (lpr_linux_stat_t *)statbuf;
    lpr_memset(st, 0, sizeof(*st));
    st->st_dev = 1;
    st->st_ino = wire->inode_number != 0 ?
        wire->inode_number :
        (wire->handle != 0 ? wire->handle : 1);
    st->st_nlink = wire->nlink != 0 ? wire->nlink : 1;
    st->st_mode = (uint32_t)wire->mode;
    st->st_size = (int64_t)wire->size;
    st->st_blksize = 4096;
    st->st_blocks = (int64_t)wire->blocks;
    st->st_rdev = wire->rdev;
    st->st_atime_sec = wire->atime_sec;
    st->st_atime_nsec = wire->atime_nsec;
    st->st_mtime_sec = wire->mtime_sec;
    st->st_mtime_nsec = wire->mtime_nsec;
    st->st_ctime_sec = wire->ctime_sec;
    st->st_ctime_nsec = wire->ctime_nsec;
}

int64_t lpr_backend_fstat(uint64_t fd, uint64_t statbuf)
{
    if (statbuf == 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (lpr_linux_device_fd_active(fd)) {
        const lpr_device_backend_t *device = lpr_device_backend(fd);
        lpr_linux_stat_t *st = (lpr_linux_stat_t *)(uintptr_t)statbuf;
        lpr_memset(st, 0, sizeof(*st));
        st->st_ino = fd + 1u;
        st->st_nlink = 1;
        st->st_mode = LPR_LINUX_S_IFCHR | 0666u;
        st->st_rdev = ((uint64_t)device->major << 8u) | device->minor;
        st->st_blksize = 4096;
        return 0;
    }
    if (lpr_linux_eventfd_active(fd) || lpr_linux_inotify_active(fd) ||
        lpr_linux_timerfd_active(fd) ||
        lpr_linux_signalfd_active(fd)) {
        lpr_linux_stat_t *st = (lpr_linux_stat_t *)(uintptr_t)statbuf;
        lpr_memset(st, 0, sizeof(*st));
        st->st_ino = fd + 1u;
        st->st_nlink = 1;
        st->st_mode = LPR_LINUX_S_IFIFO | 0600u;
        st->st_blksize = 4096;
        return 0;
    }
    if (lpr_linux_sync_file_fd_active(fd)) {
        lpr_linux_stat_t *st = (lpr_linux_stat_t *)(uintptr_t)statbuf;
        lpr_memset(st, 0, sizeof(*st));
        st->st_ino = 0x73796e630000ull + fd;
        st->st_nlink = 1;
        st->st_mode = LPR_LINUX_S_IFREG | 0600u;
        st->st_blksize = 4096;
        return 0;
    }
    if (lpr_linux_input_fd_active(fd)) {
        const lpr_input_backend_t *input = lpr_input_backend(fd);
        lpr_linux_stat_t *st = (lpr_linux_stat_t *)(uintptr_t)statbuf;
        lpr_memset(st, 0, sizeof(*st));
        st->st_ino = 0x696e7000ull + input->event_index;
        st->st_nlink = 1;
        st->st_mode = LPR_LINUX_S_IFCHR | 0660u;
        st->st_rdev = (13ull << 8) | (64u + input->event_index);
        st->st_blksize = 4096;
        return 0;
    }
    if (lpr_linux_dmabuf_fd_active(fd)) {
        const lpr_dmabuf_backend_t *dmabuf = lpr_dmabuf_backend(fd);
        lpr_linux_stat_t *st = (lpr_linux_stat_t *)(uintptr_t)statbuf;
        lpr_memset(st, 0, sizeof(*st));
        st->st_ino = 0x646d6100ull + dmabuf->token;
        st->st_nlink = 1;
        st->st_mode = LPR_LINUX_S_IFREG | 0600u;
        st->st_size = (int64_t)dmabuf->size;
        st->st_blksize = 4096;
        st->st_blocks = (int64_t)((dmabuf->size + 511u) / 512u);
        return 0;
    }
    if (lpr_linux_drm_fd_active(fd)) {
        const lpr_drm_backend_t *drm = lpr_drm_backend(fd);
        lpr_linux_stat_t *st = (lpr_linux_stat_t *)(uintptr_t)statbuf;
        lpr_memset(st, 0, sizeof(*st));
        st->st_ino = 0x64726900ull + fd;
        st->st_nlink = 1;
        st->st_mode = LPR_LINUX_S_IFCHR | 0660u;
        st->st_rdev = (226ull << 8) |
            (drm != 0 && drm->node_kind == LPR_DRM_NODE_RENDER ? 128u : 0u);
        st->st_blksize = 4096;
        return 0;
    }
    if (lpr_linux_tty_fd_active(fd)) {
        lpr_linux_stat_t *st = (lpr_linux_stat_t *)(uintptr_t)statbuf;
        lpr_memset(st, 0, sizeof(*st));
        st->st_ino = 0x74747900ull + fd;
        st->st_nlink = 1;
        st->st_mode = LPR_LINUX_S_IFCHR | 0620u;
        st->st_rdev = 0x8800ull;
        st->st_blksize = 4096;
        return 0;
    }
    if (lpr_pipe_fd_is_active(fd)) {
        lpr_linux_stat_t *st = (lpr_linux_stat_t *)(uintptr_t)statbuf;
        lpr_memset(st, 0, sizeof(*st));
        st->st_ino = fd + 1u;
        st->st_nlink = 1;
        st->st_mode = LPR_LINUX_S_IFIFO | 0600u;
        st->st_blksize = 4096;
        return 0;
    }
    if (!lpr_fd_is_filed(fd)) {
        return -LPR_LINUX_EBADF;
    }
    void *page = 0;
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    lpr_trace_process_event("fstat_filed_begin", fd, (uint64_t)(uint32_t)page_fd, 0);
    filed_statx_t *stat = (filed_statx_t *)page;
    lpr_memset(stat, 0, sizeof(*stat));
    stat->handle = lpr_filed_backend(fd)->handle;
    uint64_t ignored = 0;
    const int64_t status = lpr_filed_call(FILED_OP_VFS_STAT, page_fd, 0, &ignored);
    lpr_trace_process_event("fstat_filed_end", fd, (uint64_t)(uint32_t)page_fd, status);
    if (status == 0) {
        lpr_filed_backend_t *file = lpr_filed_backend(fd);
        if (file != 0) {
            file->stat_size = stat->size;
            file->object_generation = stat->object_generation;
        }
        lpr_write_linux_stat((void *)(uintptr_t)statbuf, stat);
    }
    lpr_destroy_wire_page(page_fd, page);
    return status;
}

static int64_t lpr_filed_fstat_close_metadata(uint64_t fd, uint64_t statbuf)
{
    /* Keep metadata and close as individually acknowledged operations.  The
     * former two-entry fast-session batch had an unrecoverable ambiguity: if
     * its combined reply was malformed or lost after Filed consumed CLOSE,
     * the client abandoned the whole session.  That teardown also destroyed
     * unrelated live handles owned by the session while their Linux FD-table
     * entries remained active, so a later fork observed a valid local FD with
     * a stale Filed handle.  A private newfstatat staging FD does not justify
     * weakening every other open file description in the process. */
    const int64_t stat_status = lpr_linux_fstat(fd, statbuf);
    (void)lpr_linux_close(fd);
    return stat_status;
}

int64_t lpr_linux_newfstatat(uint64_t dirfd, uint64_t path_raw, uint64_t statbuf, uint64_t flags)
{
    if (statbuf == 0) {
        return -LPR_LINUX_EFAULT;
    }
    lpr_trace_process_event("newfstatat_begin", dirfd, flags, 0);
    const char *path = (const char *)(uintptr_t)path_raw;
    if ((flags & LPR_LINUX_AT_EMPTY_PATH) != 0 && path != 0 && path[0] == 0) {
        const uint64_t empty_known_flags = LPR_LINUX_AT_EMPTY_PATH | LPR_LINUX_AT_SYMLINK_NOFOLLOW;
        if ((flags & ~empty_known_flags) != 0) {
            return -LPR_LINUX_EINVAL;
        }
        return lpr_linux_fstat(dirfd, statbuf);
    }
    uint64_t open_flags = LPR_LINUX_O_RDONLY;
    if ((flags & LPR_LINUX_AT_SYMLINK_NOFOLLOW) != 0) {
        open_flags |= LPR_LINUX_O_NOFOLLOW;
    }
    int64_t fd = lpr_linux_openat(dirfd, path_raw, open_flags, 0);
    /* ENOENT is definitive: adding O_DIRECTORY cannot make a missing path
     * appear, and toolkit startup probes many optional paths.  Only EISDIR
     * means the first open found an object that needs a directory handle. */
    if (fd == -LPR_LINUX_EISDIR) {
        fd = lpr_linux_openat(
            dirfd, path_raw, open_flags | LPR_LINUX_O_DIRECTORY, 0);
    }
    lpr_trace_process_event("newfstatat_open", dirfd, flags, fd);
    if (fd < 0) {
        return fd;
    }
    if (lpr_fd_is_filed((uint64_t)fd)) {
        return lpr_filed_fstat_close_metadata((uint64_t)fd, statbuf);
    }
    const int64_t status = lpr_linux_fstat((uint64_t)fd, statbuf);
    lpr_trace_process_event("newfstatat_fstat", (uint64_t)fd, flags, status);
    (void)lpr_linux_close((uint64_t)fd);
    return status;
}

static uint32_t lpr_linux_dev_major(uint64_t dev)
{
    return (uint32_t)(((dev >> 8u) & 0xfffull) | ((dev >> 32u) & 0xfffff000ull));
}

static uint32_t lpr_linux_dev_minor(uint64_t dev)
{
    return (uint32_t)((dev & 0xffull) | ((dev >> 12u) & 0xffffff00ull));
}

int64_t lpr_linux_statx(
    uint64_t dirfd,
    uint64_t path_raw,
    uint64_t flags,
    uint64_t mask,
    uint64_t statxbuf)
{
    enum {
        LPR_STATX_TYPE = 0x00000001u,
        LPR_STATX_MODE = 0x00000002u,
        LPR_STATX_NLINK = 0x00000004u,
        LPR_STATX_UID = 0x00000008u,
        LPR_STATX_GID = 0x00000010u,
        LPR_STATX_ATIME = 0x00000020u,
        LPR_STATX_MTIME = 0x00000040u,
        LPR_STATX_CTIME = 0x00000080u,
        LPR_STATX_INO = 0x00000100u,
        LPR_STATX_SIZE = 0x00000200u,
        LPR_STATX_BLOCKS = 0x00000400u,
        LPR_STATX_BASIC_STATS = 0x000007ffu,
    };
    const uint64_t known_flags =
        LPR_LINUX_AT_SYMLINK_NOFOLLOW |
        LPR_LINUX_AT_EMPTY_PATH |
        0x800ull | 0x4000ull;
    if (statxbuf == 0) {
        return -LPR_LINUX_EFAULT;
    }
    if ((flags & ~known_flags) != 0 || (mask & 0x80000000ull) != 0) {
        return -LPR_LINUX_EINVAL;
    }

    lpr_linux_stat_t st;
    lpr_memset(&st, 0, sizeof(st));
    const int64_t status = lpr_linux_newfstatat(
        dirfd,
        path_raw,
        (uint64_t)(uintptr_t)&st,
        flags & (LPR_LINUX_AT_SYMLINK_NOFOLLOW | LPR_LINUX_AT_EMPTY_PATH));
    if (status != 0) {
        return status;
    }

    lpr_linux_statx_t *out = (lpr_linux_statx_t *)(uintptr_t)statxbuf;
    lpr_memset(out, 0, sizeof(*out));
    out->stx_mask = LPR_STATX_BASIC_STATS;
    out->stx_blksize = st.st_blksize > 0 ? (uint32_t)st.st_blksize : 4096u;
    out->stx_nlink = st.st_nlink > UINT32_MAX ? UINT32_MAX : (uint32_t)st.st_nlink;
    out->stx_uid = st.st_uid;
    out->stx_gid = st.st_gid;
    out->stx_mode = (uint16_t)st.st_mode;
    out->stx_ino = st.st_ino;
    out->stx_size = st.st_size < 0 ? 0 : (uint64_t)st.st_size;
    out->stx_blocks = st.st_blocks < 0 ? 0 : (uint64_t)st.st_blocks;
    out->stx_atime.tv_sec = st.st_atime_sec;
    out->stx_atime.tv_nsec = (uint32_t)st.st_atime_nsec;
    out->stx_mtime.tv_sec = st.st_mtime_sec;
    out->stx_mtime.tv_nsec = (uint32_t)st.st_mtime_nsec;
    out->stx_ctime.tv_sec = st.st_ctime_sec;
    out->stx_ctime.tv_nsec = (uint32_t)st.st_ctime_nsec;
    out->stx_rdev_major = lpr_linux_dev_major(st.st_rdev);
    out->stx_rdev_minor = lpr_linux_dev_minor(st.st_rdev);
    out->stx_dev_major = lpr_linux_dev_major(st.st_dev);
    out->stx_dev_minor = lpr_linux_dev_minor(st.st_dev);
    (void)mask;
    (void)LPR_STATX_TYPE;
    (void)LPR_STATX_MODE;
    (void)LPR_STATX_NLINK;
    (void)LPR_STATX_UID;
    (void)LPR_STATX_GID;
    (void)LPR_STATX_ATIME;
    (void)LPR_STATX_MTIME;
    (void)LPR_STATX_CTIME;
    (void)LPR_STATX_INO;
    (void)LPR_STATX_SIZE;
    (void)LPR_STATX_BLOCKS;
    return 0;
}

int64_t lpr_linux_access(uint64_t path, uint64_t mode)
{
    return lpr_linux_faccessat((uint64_t)(int64_t)LPR_LINUX_AT_FDCWD, path, mode, 0);
}

int64_t lpr_linux_faccessat(uint64_t dirfd, uint64_t path, uint64_t mode, uint64_t flags)
{
    const uint64_t known_mode = 0x7ull;
    const uint64_t known_flags =
        LPR_LINUX_AT_SYMLINK_NOFOLLOW | LPR_LINUX_AT_EACCESS | LPR_LINUX_AT_EMPTY_PATH;
    if ((mode & ~known_mode) != 0 || (flags & ~known_flags) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    struct lpr_linux_stat statbuf;
    lpr_memset(&statbuf, 0, sizeof(statbuf));
    return lpr_linux_newfstatat(
        dirfd, path, (uint64_t)(uintptr_t)&statbuf, flags & ~LPR_LINUX_AT_EACCESS);
}

int64_t lpr_linux_open_metadata(uint64_t dirfd, uint64_t path_raw, uint64_t flags)
{
    uint64_t open_flags = LPR_LINUX_O_RDONLY;
    if ((flags & LPR_LINUX_AT_SYMLINK_NOFOLLOW) != 0) {
        open_flags |= LPR_LINUX_O_NOFOLLOW;
    }
    int64_t fd = lpr_linux_openat(dirfd, path_raw, open_flags, 0);
    if (fd == -LPR_LINUX_EISDIR) {
        fd = lpr_linux_openat(
            dirfd,
            path_raw,
            open_flags | LPR_LINUX_O_DIRECTORY,
            0);
    }
    return fd;
}

int64_t lpr_linux_fchmod(uint64_t fd, uint64_t mode)
{
    if (!lpr_fd_is_filed(fd)) {
        return -LPR_LINUX_EBADF;
    }
    void *page = 0;
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    filed_chmod_t *chmod_req = (filed_chmod_t *)page;
    lpr_memset(chmod_req, 0, sizeof(*chmod_req));
    chmod_req->handle = lpr_filed_backend(fd)->handle;
    chmod_req->mode = mode & 07777ull;
    uint64_t ignored = 0;
    const int64_t status = lpr_filed_call(FILED_OP_VFS_CHMOD, page_fd, 0, &ignored);
    lpr_destroy_wire_page(page_fd, page);
    return status;
}

int64_t lpr_linux_fchmodat(uint64_t dirfd, uint64_t path_raw, uint64_t mode, uint64_t flags)
{
    const uint64_t known_flags = LPR_LINUX_AT_SYMLINK_NOFOLLOW | LPR_LINUX_AT_EMPTY_PATH;
    if ((flags & ~known_flags) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    const char *path = (const char *)(uintptr_t)path_raw;
    if ((flags & LPR_LINUX_AT_EMPTY_PATH) != 0 && path != 0 && path[0] == 0) {
        return lpr_linux_fchmod(dirfd, mode);
    }
    if (path == 0) {
        return -LPR_LINUX_EFAULT;
    }
    const int64_t fd = lpr_linux_open_metadata(dirfd, path_raw, flags);
    if (fd < 0) {
        return fd;
    }
    const int64_t status = lpr_linux_fchmod((uint64_t)fd, mode);
    (void)lpr_linux_close((uint64_t)fd);
    return status;
}

int64_t lpr_linux_now(lpr_linux_timespec_t *out)
{
    if (out == 0) {
        return -LPR_LINUX_EFAULT;
    }
    lpr_memset(out, 0, sizeof(*out));
    const int64_t status = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_CLOCK_GETTIME,
        0,
        (uint64_t)(uintptr_t)out);
    return lpr_pacha_status_to_errno(status);
}

int lpr_linux_utimens_both_omit(const lpr_linux_timespec_t *times)
{
    return times != 0 &&
        times[0].tv_nsec == LPR_LINUX_UTIME_OMIT &&
        times[1].tv_nsec == LPR_LINUX_UTIME_OMIT;
}

int64_t lpr_linux_utimens_plan(
    const lpr_linux_timespec_t *times,
    lpr_linux_utimens_plan_t *out_plan)
{
    if (out_plan == 0) {
        return -LPR_LINUX_EFAULT;
    }
    out_plan->mask = 0;
    out_plan->needs_now = 0;
    if (times == 0) {
        out_plan->mask = FILED_UTIMENS_ATIME | FILED_UTIMENS_MTIME;
        out_plan->needs_now = 1;
        return 0;
    }

    const uint64_t bits[2] = { FILED_UTIMENS_ATIME, FILED_UTIMENS_MTIME };
    for (uint32_t i = 0; i < 2; ++i) {
        const int64_t nsec = times[i].tv_nsec;
        if (nsec == LPR_LINUX_UTIME_OMIT) {
            continue;
        }
        if (nsec == LPR_LINUX_UTIME_NOW) {
            out_plan->mask |= bits[i];
            out_plan->needs_now = 1;
            continue;
        }
        if (nsec < 0 || nsec >= 1000000000ll) {
            return -LPR_LINUX_EINVAL;
        }
        out_plan->mask |= bits[i];
    }
    return 0;
}

int64_t lpr_linux_resolve_utime(
    const lpr_linux_timespec_t *input,
    const lpr_linux_timespec_t *now,
    uint64_t wire_bit,
    uint64_t *mask,
    int64_t *out_sec,
    int64_t *out_nsec)
{
    if (mask == 0 || out_sec == 0 || out_nsec == 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (input == 0) {
        if (now == 0) {
            return -LPR_LINUX_EFAULT;
        }
        *mask |= wire_bit;
        *out_sec = now->tv_sec;
        *out_nsec = now->tv_nsec;
        return 0;
    }
    if (input->tv_nsec == LPR_LINUX_UTIME_OMIT) {
        return 0;
    }
    *mask |= wire_bit;
    if (input->tv_nsec == LPR_LINUX_UTIME_NOW) {
        if (now == 0) {
            return -LPR_LINUX_EFAULT;
        }
        *out_sec = now->tv_sec;
        *out_nsec = now->tv_nsec;
        return 0;
    }
    if (input->tv_nsec < 0 || input->tv_nsec >= 1000000000ll) {
        return -LPR_LINUX_EINVAL;
    }
    *out_sec = input->tv_sec;
    *out_nsec = input->tv_nsec;
    return 0;
}

int64_t lpr_filed_utimens_handle(uint64_t handle, uint64_t times_raw)
{
    const lpr_linux_timespec_t *times = (const lpr_linux_timespec_t *)(uintptr_t)times_raw;
    lpr_linux_utimens_plan_t plan;
    lpr_linux_timespec_t now;
    const lpr_linux_timespec_t *now_ptr = 0;
    uint64_t mask = 0;
    int64_t status = lpr_linux_utimens_plan(times, &plan);
    if (status != 0) {
        return status;
    }
    if (plan.mask == 0) {
        return 0;
    }
    if (plan.needs_now != 0) {
        status = lpr_linux_now(&now);
        if (status != 0) {
            return status;
        }
        now_ptr = &now;
    }

    void *page = 0;
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    filed_utimens_t *utimens = (filed_utimens_t *)page;
    lpr_memset(utimens, 0, sizeof(*utimens));
    utimens->handle = handle;

    status = lpr_linux_resolve_utime(
        times_raw == 0 ? 0 : &times[0],
        now_ptr,
        FILED_UTIMENS_ATIME,
        &mask,
        &utimens->atime_sec,
        &utimens->atime_nsec);
    if (status == 0) {
        status = lpr_linux_resolve_utime(
            times_raw == 0 ? 0 : &times[1],
            now_ptr,
            FILED_UTIMENS_MTIME,
            &mask,
            &utimens->mtime_sec,
            &utimens->mtime_nsec);
    }
    uint64_t ignored = 0;
    if (status == 0 && mask != 0) {
        utimens->mask = mask;
        status = lpr_filed_call(FILED_OP_VFS_UTIMENS, page_fd, 0, &ignored);
    }
    lpr_destroy_wire_page(page_fd, page);
    return status;
}

int64_t lpr_linux_utimensat(uint64_t dirfd, uint64_t path_raw, uint64_t times, uint64_t flags)
{
    const lpr_linux_timespec_t *time_values =
        (const lpr_linux_timespec_t *)(uintptr_t)times;
    if (lpr_linux_utimens_both_omit(time_values)) {
        return 0;
    }
    const uint64_t known_flags = LPR_LINUX_AT_SYMLINK_NOFOLLOW | LPR_LINUX_AT_EMPTY_PATH;
    if ((flags & ~known_flags) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    const char *path = (const char *)(uintptr_t)path_raw;
    if (path == 0) {
        if (flags != 0) {
            return -LPR_LINUX_EINVAL;
        }
        const uint64_t handle = lpr_linux_filed_fd_handle(dirfd);
        if (handle == 0) {
            return lpr_fd_linux_visible_active(dirfd) ?
                -LPR_LINUX_EOPNOTSUPP : -LPR_LINUX_EBADF;
        }
        return lpr_filed_utimens_handle(handle, times);
    }
    if ((flags & LPR_LINUX_AT_EMPTY_PATH) != 0 && path != 0 && path[0] == 0) {
        const uint64_t handle = lpr_linux_filed_fd_handle(dirfd);
        if (handle == 0) {
            return lpr_fd_linux_visible_active(dirfd) ?
                -LPR_LINUX_EOPNOTSUPP : -LPR_LINUX_EBADF;
        }
        return lpr_filed_utimens_handle(handle, times);
    }
    const int64_t fd = lpr_linux_open_metadata(dirfd, path_raw, flags);
    if (fd < 0) {
        return fd;
    }
    const uint64_t handle = lpr_linux_filed_fd_handle((uint64_t)fd);
    const int64_t status = handle != 0 ?
        lpr_filed_utimens_handle(handle, times) : -LPR_LINUX_EOPNOTSUPP;
    (void)lpr_linux_close((uint64_t)fd);
    return status;
}

int64_t lpr_linux_readlink(uint64_t path, uint64_t buf, uint64_t bufsiz)
{
    if (buf == 0 && bufsiz != 0) {
        return -LPR_LINUX_EFAULT;
    }
    const char *path_string = (const char *)(uintptr_t)path;
    const uint64_t path_len = path_string != 0 ?
        (uint64_t)lpr_strnlen(path_string, FILED_PATH_BYTES) :
        0;
    int64_t cached_status = 0;
    if (lpr_readlink_cache_lookup(path_string, path_len, &cached_status)) {
        return cached_status;
    }
    const char *trace_path = path_string;
    if (trace_path != 0) {
        pacha_trace2(
            PACHA_TRACE_COMPONENT_LPR,
            PACHA_TRACE_EVENT_LPR_PROCESS,
            PACHA_TRACE_CLASS_DEBUG,
            pacha_trace_name_id("readlink_path"),
            pacha_trace_name_id(trace_path));
    }
    char target[FILED_SYMLINK_TARGET_BYTES];
    lpr_memset(target, 0, sizeof(target));
    const int64_t status = lpr_linux_readlinkat_to_buffer(
        (uint64_t)(int64_t)LPR_LINUX_AT_FDCWD,
        path,
        target,
        sizeof(target));
    if (status < 0) {
        lpr_readlink_cache_store(path_string, path_len, status);
        return status;
    }
    uint64_t copy_len = (uint64_t)status;
    if (copy_len > bufsiz) {
        copy_len = bufsiz;
    }
    if (copy_len != 0) {
        lpr_memcpy((void *)(uintptr_t)buf, target, copy_len);
    }
    return (int64_t)copy_len;
}

uint16_t lpr_dirent_reclen(uint64_t name_len)
{
    const uint64_t raw = 19u + name_len + 1u;
    return (uint16_t)((raw + 7u) & ~7ull);
}

int64_t lpr_linux_getdents64(uint64_t fd, uint64_t buf, uint64_t count)
{
    if (!lpr_fd_is_filed(fd)) {
        return -LPR_LINUX_EBADF;
    }
    if (buf == 0 && count != 0) {
        return -LPR_LINUX_EFAULT;
    }
    const int64_t proc_status = lpr_linux_proc_getdents64(fd, buf, count);
    if (proc_status != -LPR_LINUX_ENOENT) {
        return proc_status;
    }
    void *page = 0;
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    filed_getdents_t *gd = (filed_getdents_t *)page;
    lpr_memset(gd, 0, sizeof(*gd));
    gd->dir_handle = lpr_filed_backend(fd)->handle;
    gd->capacity = FILED_DIRENT_CAPACITY;
    uint64_t ignored = 0;
    int64_t status = lpr_filed_call(FILED_OP_VFS_GETDENTS, page_fd, 0, &ignored);
    if (status != 0) {
        lpr_destroy_wire_page(page_fd, page);
        return status;
    }

    uint8_t *out = (uint8_t *)(uintptr_t)buf;
    uint64_t written = 0;
    for (uint64_t i = 0; i < gd->count && i < FILED_DIRENT_CAPACITY; i += 1) {
        const filed_dirent_t *entry = &gd->entries[i];
        const uint64_t name_len = entry->name_len < FILED_DIRENT_NAME_BYTES ?
            entry->name_len :
            FILED_DIRENT_NAME_BYTES - 1u;
        const uint16_t reclen = lpr_dirent_reclen(name_len);
        if (written + reclen > count) {
            break;
        }
        lpr_memset(out + written, 0, reclen);
        uint64_t next_offset = gd->offset + i + 1u;
        if (next_offset <= gd->offset) {
            next_offset = 0x7fffffffffffffffull;
        }
        *(uint64_t *)(void *)(out + written + 0u) = entry->handle != 0 ? entry->handle : next_offset;
        *(int64_t *)(void *)(out + written + 8u) =
            next_offset > 0x7fffffffffffffffull ? (int64_t)0x7fffffffffffffffull : (int64_t)next_offset;
        *(uint16_t *)(void *)(out + written + 16u) = reclen;
        *(uint8_t *)(void *)(out + written + 18u) = lpr_dtype_from_mode(entry->kind);
        lpr_memcpy(out + written + 19u, entry->name, (size_t)name_len);
        out[written + 19u + name_len] = 0;
        written += reclen;
    }
    lpr_destroy_wire_page(page_fd, page);
    return (int64_t)written;
}
