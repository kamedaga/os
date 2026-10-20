#include "../lpr_filed_internal.h"
#include "../lpr_gui_detail.h"
#include "../support/browser_diag.h"
#include "../support/mmap_profile.h"

static int64_t lpr_linux_file_vmo_call(uint32_t op, uint64_t flags,
    uint64_t fd, uint64_t file_offset, uint64_t length, uint64_t *out_loaded)
{
    if (out_loaded) *out_loaded = 0;
    const int64_t binding_status = lpr_filed_fd_binding(fd);
    if (binding_status) return binding_status;
    const lpr_filed_backend_t *file = lpr_filed_backend(fd);
    if (!file) return -LPR_LINUX_EBADF;
    kb2_fs_request_t request = {.opcode = op, .handle = file->handle,
        .offset = (int64_t)file_offset, .length = length, .flags = flags};
    kb2_fs_response_t response;
    pacha_fd_t returned = -1;
    const int64_t status = lpr_fs_call(&request, 0, 0, 0, 0, 0, 0,
        &response, -1, &returned);
    if (status < 0) return status;
    if (returned < 16) return -LPR_LINUX_EIO;
    if (out_loaded) *out_loaded = (uint64_t)response.result;
    return returned;
}

int64_t lpr_linux_file_vmo(uint64_t fd, uint64_t file_offset, uint64_t length, int executable, uint64_t *out_loaded)
{
    return lpr_linux_file_vmo_call(
        KB2_FILESYSTEM_OP_FILE_VMO,
        executable ? FILED_FILE_VMO_EXEC : 0,
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
        KB2_FILESYSTEM_OP_SHARED_FILE_VMO,
        (writable ? FILED_FILE_VMO_WRITE : 0) |
            (executable ? FILED_FILE_VMO_EXEC : 0),
        fd,
        file_offset,
        length,
        out_file_size);
}

int64_t lpr_backend_write(uint64_t fd, uint64_t buf, uint64_t count, unsigned *wait_not_ready __attribute__((unused)))
{
    if (lpr_block_backend(fd)) {
        const uint64_t offset = lpr_filed_control_offset(fd);
        const int64_t n = lpr_block_io(fd, buf, count, offset, 1);
        if (n > 0) lpr_filed_control_advance_offset(fd, offset, (uint64_t)n);
        return n;
    }
    if (lpr_linux_device_fd_active(fd)) {
        if ((lpr_device_backend(fd)->flags & LPR_LINUX_O_ACCMODE) == LPR_LINUX_O_RDONLY) {
            return -LPR_LINUX_EBADF;
        }
        (void)buf;
        return count <= INT64_MAX ? (int64_t)count : -LPR_LINUX_EINVAL;
    }
    if (lpr_linux_tty_fd_active(fd)) {
        return lpr_tty_io(TERMD_OP_HANDLE_WRITE, fd, buf, count, wait_not_ready);
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
                wait_status = lpr_wait_graph_block(&graph, &deadline, wait_not_ready);
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
                (uint64_t)pipe->native.raw,
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
            const int64_t wait_status = lpr_pipe_wait(fd, 0x0004u, min_write, wait_not_ready);
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
        }
        return lpr_filed_io(KB2_FILESYSTEM_OP_WRITE, fd, buf, count, 0);
    }
    return -LPR_LINUX_EBADF;
}

int64_t lpr_backend_writev(uint64_t fd, uint64_t iov_raw, uint64_t iov_count, unsigned *wait_not_ready __attribute__((unused)))
{
    if (iov_raw == 0 && iov_count != 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (lpr_block_backend(fd))
        return lpr_iov_scalar_io(fd, iov_raw, iov_count, 1, wait_not_ready);
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
        return lpr_iov_scalar_io(fd, iov_raw, iov_count, 1, wait_not_ready);
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
                    (uint64_t)pipe->native.raw,
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
                const int64_t wait_status = lpr_pipe_wait(fd, 0x0004u, min_write, wait_not_ready);
                if (wait_status != 0) {
                    return wait_status;
                }
            }
        }
        if (lpr_linux_eventfd_active(fd)) {
            int64_t total = 0;
            const lpr_linux_iovec_t *iov = (const lpr_linux_iovec_t *)(uintptr_t)iov_raw;
            for (uint64_t i = 0; i < iov_count; i += 1) {
                const int64_t n = lpr_linux_write(fd, iov[i].base, iov[i].len, wait_not_ready);
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
    const int64_t binding_status = lpr_filed_endpoint_ready();
    if (binding_status) return binding_status;
    const lpr_filed_backend_t *file = lpr_filed_backend(fd);
    return file ? lpr_fs_iov(file->handle, iov_raw, iov_count, 1) : -LPR_LINUX_EBADF;
}

int64_t lpr_linux_close(uint64_t fd)
{
    if (fd > LPR_LINUX_FD_MAX || !lpr_control_fd_active(fd)) {
        return -LPR_LINUX_EBADF;
    }
    lpr_browser_diag("close", fd, 0, 0);
    return lpr_control_close_fd_status(fd);
}

int64_t lpr_linux_close_range(uint64_t first, uint64_t last, uint64_t flags)
{
    lpr_browser_diag("close-range", first, last, flags);
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
#if defined(LPR_GUI_PROFILE) && LPR_GUI_PROFILE
    const int gui_profile = lpr_gui_profile_current_thread();
    uint64_t gui_begin = gui_profile ? pacha_trace_read_tsc() : 0;
#define GUI_SEEK_MARK(k) do { if (gui_profile) { \
    const uint64_t end = pacha_trace_read_tsc(); \
    lpr_gui_profile_span((k), gui_begin, end); gui_begin = pacha_trace_read_tsc(); \
} } while (0)
#else
#define GUI_SEEK_MARK(k) ((void)0)
#endif
    if (fd > LPR_LINUX_FD_MAX) {
        return -LPR_LINUX_ESPIPE;
    }
    lpr_fd_pin_t pin;
    if (lpr_fd_table_pin(&lpr_control_fd_table, (uint32_t)fd, &pin) != 0) {
        return -LPR_LINUX_ESPIPE;
    }

    int64_t result = -LPR_LINUX_ESPIPE;
    GUI_SEEK_MARK(0);
    if (pin.ops_id == LPR_FD_OPS_INSTALL_DISK) {
        uint64_t next = 0;
        if (whence <= 1) {
            result = lpr_fd_table_seek_pinned(&lpr_control_fd_table,
                &pin, (int64_t)offset, (uint32_t)whence, &next) == 0 &&
                next <= INT64_MAX ? (int64_t)next : -LPR_LINUX_EINVAL;
        } else if (whence == 2) {
            const int64_t size = lpr_block_size(fd);
            const int64_t delta = (int64_t)offset;
            if (size >= 0 && delta >= -size &&
                (delta <= 0 || size <= INT64_MAX - delta)) {
                next = (uint64_t)(size + delta);
                lpr_filed_control_set_offset(fd, next);
                result = (int64_t)next;
            } else result = -LPR_LINUX_EINVAL;
        } else result = -LPR_LINUX_EINVAL;
        goto out;
    }
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
    result = lpr_filed_endpoint_ready();
    if (result) goto out;

    lpr_filed_backend_t *filed = (lpr_filed_backend_t *)pin.state;
#if defined(LPR_GUI_PROFILE) && LPR_GUI_PROFILE
    if (gui_profile) {
        lpr_gui_profile_file(8, whence, filed->flags, 0,
            0, 0, 0);
        const uint64_t tick = pacha_trace_read_tsc();
        lpr_gui_profile_span(whence <= 2 ? 8u+(unsigned)whence : 8u, tick, tick+1u);
    }
#endif
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_LSEEK,
        .handle = filed->handle, .offset = (int64_t)offset, .flags = whence};
    kb2_fs_response_t response;
    result = lpr_fs_call(&request, 0, 0, 0, 0, 0, 0, &response, -1, 0);

out:
    GUI_SEEK_MARK(5);
    lpr_fd_unpin(&pin);
    GUI_SEEK_MARK(6);
#undef GUI_SEEK_MARK
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
    if (lpr_linux_epoll_fd_active(fd) || lpr_unix_socket_active(fd)) {
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
    case LPR_LINUX_F_GETFD: return lpr_control_get_fd_flags(fd);
    case LPR_LINUX_F_SETFD: return lpr_control_set_fd_flags(fd, arg);
    case LPR_LINUX_F_DUPFD:
    case LPR_LINUX_F_DUPFD_CLOEXEC:
        return lpr_linux_dup(fd, arg, cmd == LPR_LINUX_F_DUPFD_CLOEXEC);
    default: {
        const int64_t binding_status = lpr_filed_endpoint_ready();
        if (binding_status) return binding_status;
        const lpr_filed_backend_t *file = lpr_filed_backend(fd);
        if (!file) return -LPR_LINUX_EBADF;
        kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_FCNTL,
            .handle = file->handle, .flags = cmd, .length = arg};
        kb2_fs_response_t response;
        return lpr_fs_call(&request, 0, 0, 0, 0, 0, 0, &response, -1, 0);
    }
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

int64_t lpr_backend_ioctl(uint64_t fd, uint64_t request, uint64_t arg, unsigned *wait_not_ready __attribute__((unused)))
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
    if (lpr_block_backend(fd)) {
        return lpr_block_ioctl(fd, request, arg);
    }
    if (lpr_linux_input_fd_active(fd)) {
        return lpr_input_ioctl(fd, request, arg);
    }
    if (lpr_linux_drm_fd_active(fd)) {
        return lpr_drm_ioctl(fd, request, arg, wait_not_ready);
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

static uint32_t fs_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t fs_get64(const uint8_t *p)
{
    return fs_get32(p) | (uint64_t)fs_get32(p + 4) << 32;
}

static uint64_t fs_dev(uint32_t major, uint32_t minor)
{
    return (minor & 0xffu) | (uint64_t)(major & 0xfffu) << 8 |
        (uint64_t)(minor & ~0xffu) << 12 | (uint64_t)(major & ~0xfffu) << 32;
}

void lpr_write_linux_stat(void *statbuf, const uint8_t *wire)
{
    lpr_linux_stat_t *st = statbuf;
    lpr_memset(st, 0, sizeof(*st));
#define S32(field) fs_get32(wire + KB2_FILESYSTEM_STATX_##field##_OFFSET)
#define S64(field) fs_get64(wire + KB2_FILESYSTEM_STATX_##field##_OFFSET)
    st->st_dev = fs_dev(S32(DEV_MAJOR), S32(DEV_MINOR));
    st->st_ino = S64(INO);
    st->st_nlink = S32(NLINK);
    st->st_mode = wire[KB2_FILESYSTEM_STATX_MODE_OFFSET] |
        (uint32_t)wire[KB2_FILESYSTEM_STATX_MODE_OFFSET + 1] << 8;
    st->st_uid = S32(UID); st->st_gid = S32(GID);
    st->st_rdev = fs_dev(S32(RDEV_MAJOR), S32(RDEV_MINOR));
    st->st_size = (int64_t)S64(SIZE);
    st->st_blksize = S32(BLKSIZE);
    st->st_blocks = (int64_t)S64(BLOCKS);
    st->st_atime_sec = (int64_t)S64(ATIME); st->st_atime_nsec = fs_get32(wire + KB2_FILESYSTEM_STATX_ATIME_OFFSET + 8);
    st->st_mtime_sec = (int64_t)S64(MTIME); st->st_mtime_nsec = fs_get32(wire + KB2_FILESYSTEM_STATX_MTIME_OFFSET + 8);
    st->st_ctime_sec = (int64_t)S64(CTIME); st->st_ctime_nsec = fs_get32(wire + KB2_FILESYSTEM_STATX_CTIME_OFFSET + 8);
#undef S32
#undef S64
}

static int64_t fs_stat(uint64_t handle, const char *path, uint64_t flags,
    uint64_t mask, uint8_t *wire)
{
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_STATX, .handle = handle,
        .flags = flags, .mask = (uint32_t)mask};
    kb2_fs_response_t response;
    const int64_t result = lpr_fs_call(&request, path, 0, 0, 0, wire,
        KB2_FILESYSTEM_STATX_SIZE, &response, -1, 0);
    if (!result && response.data_length != KB2_FILESYSTEM_STATX_SIZE) return -LPR_LINUX_EIO;
    return result;
}


int64_t lpr_filed_fstat_pinned(const lpr_fd_pin_t *pin, uint64_t statbuf)
{
    if (!statbuf) return -LPR_LINUX_EFAULT;
    const int64_t binding_status = lpr_filed_endpoint_ready();
    if (binding_status) return binding_status;
    uint8_t wire[KB2_FILESYSTEM_STATX_SIZE];
    const lpr_filed_backend_t *file = pin->state;
    const int64_t status = fs_stat(file->handle, "", LPR_LINUX_AT_EMPTY_PATH, 0x7ffu, wire);
    if (!status) lpr_write_linux_stat((void *)(uintptr_t)statbuf, wire);
    return status;
}

int64_t lpr_backend_fstat(uint64_t fd, uint64_t statbuf)
{
    if (statbuf == 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (lpr_block_backend(fd)) {
        lpr_linux_stat_t *st = (lpr_linux_stat_t *)(uintptr_t)statbuf;
        const int64_t size = lpr_block_size(fd);
        if (size < 0) return size;
        lpr_memset(st, 0, sizeof(*st));
        st->st_dev = 0x70616368u;
        st->st_ino = 1;
        st->st_nlink = 1;
        st->st_mode = LPR_LINUX_S_IFREG | 0600u;
        st->st_size = size;
        st->st_blksize = 4096;
        st->st_blocks = (size + 511) / 512;
        return 0;
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
        /* Linux dev_t splits minor numbers above 255 into the high bits;
         * otherwise an extended event registry changes the reported major. */
        const uint64_t minor = 64u + input->event_index;
        st->st_rdev = (13ull << 8) | (minor & 0xffu) |
            ((minor & ~0xffull) << 12);
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
        /* The virtual PTY already reports mode 0620. Expose its matching
         * conventional tty group too, so POSIX consumers need not chmod a
         * synthetic device inode through the regular FileD path. */
        st->st_gid = 5;
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
    uint8_t wire[KB2_FILESYSTEM_STATX_SIZE];
    const int64_t binding_status = lpr_filed_endpoint_ready();
    if (binding_status) return binding_status;
    const lpr_filed_backend_t *file = lpr_filed_backend(fd);
    if (!file) return -LPR_LINUX_EBADF;
    const int64_t status = fs_stat(file->handle, "",
        LPR_LINUX_AT_EMPTY_PATH, 0x7ffu, wire);
    if (!status) lpr_write_linux_stat((void *)(uintptr_t)statbuf, wire);
    return status;
}

static int64_t lpr_filed_fstat_close_metadata(uint64_t fd, uint64_t statbuf, unsigned *wait_not_ready __attribute__((unused)))
{
    /* Keep metadata and close as individually acknowledged operations.  The
     * former two-entry fast-session batch had an unrecoverable ambiguity: if
     * its combined reply was malformed or lost after Filed consumed CLOSE,
     * the client abandoned the whole session.  That teardown also destroyed
     * unrelated live handles owned by the session while their Linux FD-table
     * entries remained active, so a later fork observed a valid local FD with
     * a stale Filed handle.  A private newfstatat staging FD does not justify
     * weakening every other open file description in the process. */
    const int64_t stat_status = lpr_linux_fstat(fd, statbuf, wait_not_ready);
    (void)lpr_linux_close(fd);
    return stat_status;
}

int64_t lpr_linux_newfstatat(uint64_t dirfd, uint64_t path_raw, uint64_t statbuf, uint64_t flags, unsigned *wait_not_ready __attribute__((unused)))
{
    if (statbuf == 0) {
        return -LPR_LINUX_EFAULT;
    }
    lpr_trace_process_event("newfstatat_begin", dirfd, flags, 0);
    const char *path = (const char *)(uintptr_t)path_raw;
    if (path == 0) return -LPR_LINUX_EFAULT;
    const uint64_t known_flags = LPR_LINUX_AT_EMPTY_PATH | LPR_LINUX_AT_SYMLINK_NOFOLLOW | 0x800u; /* AT_NO_AUTOMOUNT */
    if ((flags & ~known_flags) != 0) return -LPR_LINUX_EINVAL;
    if ((flags & LPR_LINUX_AT_EMPTY_PATH) != 0 && path[0] == 0) {
        return lpr_linux_fstat(dirfd, statbuf, wait_not_ready);
    }
    if (path[0] == '\0') return -LPR_LINUX_ENOENT;
    const int64_t drm_stat = lpr_drm_stat_path(path, statbuf);
    if (drm_stat != -LPR_LINUX_ENOENT) return drm_stat;
    if (!lpr_strcmp(path, "/dev/pacha/install-target")) {
        const int64_t block_fd = lpr_block_open_path(path, LPR_LINUX_O_RDONLY);
        if (block_fd < 0) return block_fd;
        const int64_t result = lpr_backend_fstat((uint64_t)block_fd, statbuf);
        (void)lpr_linux_close((uint64_t)block_fd);
        return result;
    }
    /* Virtual proc/device descriptors belong to LPR, not the FileD namespace.
     * Keep their existing handling; regular filesystem paths need no Linux FD. */
    const int virtual_path =
        (lpr_strncmp(path, "/proc", 5) == 0 && (path[5] == '/' || path[5] == '\0')) ||
        (lpr_strncmp(path, "/dev", 4) == 0 && (path[4] == '/' || path[4] == '\0'));
    if (!virtual_path) {
        uint64_t dir_handle = 0;
        int64_t status = lpr_dir_handle_for(dirfd, path, &dir_handle);
        if (status != 0) return status;
        uint8_t wire[KB2_FILESYSTEM_STATX_SIZE];
        status = fs_stat(dir_handle, path, flags, 0x7ffu, wire);
        if (!status) lpr_write_linux_stat((void *)(uintptr_t)statbuf, wire);
        return status;
    }
    /* stat() must not acquire a controlling terminal when its virtual-device
     * fallback temporarily opens /dev/pts/N.  A plain O_RDONLY open let the
     * SSH daemon's metadata probe claim its child's future PTY. */
    uint64_t open_flags = LPR_LINUX_O_RDONLY | LPR_LINUX_O_NOCTTY;
    if ((flags & LPR_LINUX_AT_SYMLINK_NOFOLLOW) != 0) {
        open_flags |= LPR_LINUX_O_NOFOLLOW;
    }
    int64_t fd = lpr_linux_openat(dirfd, path_raw, open_flags, 0, wait_not_ready);
    /* ENOENT is definitive: adding O_DIRECTORY cannot make a missing path
     * appear, and toolkit startup probes many optional paths.  Only EISDIR
     * means the first open found an object that needs a directory handle. */
    if (fd == -LPR_LINUX_EISDIR) {
        fd = lpr_linux_openat(
            dirfd, path_raw, open_flags | LPR_LINUX_O_DIRECTORY, 0, wait_not_ready);
    }
    lpr_trace_process_event("newfstatat_open", dirfd, flags, fd);
    if (fd < 0) {
        return fd;
    }
    if (lpr_fd_is_filed((uint64_t)fd)) {
        return lpr_filed_fstat_close_metadata((uint64_t)fd, statbuf, wait_not_ready);
    }
    const int64_t status = lpr_linux_fstat((uint64_t)fd, statbuf, wait_not_ready);
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
    uint64_t statxbuf, unsigned *wait_not_ready __attribute__((unused)))
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
        0x800ull | 0x6000ull;
    if (statxbuf == 0) {
        return -LPR_LINUX_EFAULT;
    }
    if ((flags & ~known_flags) != 0 || (mask & 0x80000000ull) != 0) {
        return -LPR_LINUX_EINVAL;
    }

    const char *path = (const char *)(uintptr_t)path_raw;
    if (!path) return -LPR_LINUX_EFAULT;
    const int virtual_path = (lpr_strncmp(path, "/proc", 5) == 0 &&
        (path[5] == '/' || !path[5])) || (lpr_strncmp(path, "/dev", 4) == 0 &&
        (path[4] == '/' || !path[4]));
    if (!virtual_path) {
        uint64_t handle;
        int64_t status = (flags & LPR_LINUX_AT_EMPTY_PATH) && !path[0] ?
            0 : lpr_dir_handle_for(dirfd, path, &handle);
        if ((flags & LPR_LINUX_AT_EMPTY_PATH) && !path[0]) {
            status = lpr_filed_fd_binding(dirfd);
            handle = lpr_linux_filed_fd_handle(dirfd);
        }
        if (status) return status;
        uint8_t wire[KB2_FILESYSTEM_STATX_SIZE];
        status = fs_stat(handle, path, flags, mask, wire);
        if (!status) lpr_memcpy((void *)(uintptr_t)statxbuf, wire, sizeof(wire));
        return status;
    }
    lpr_linux_stat_t st;
    lpr_memset(&st, 0, sizeof(st));
    const int64_t status = lpr_linux_newfstatat(
        dirfd,
        path_raw,
        (uint64_t)(uintptr_t)&st,
        flags & (LPR_LINUX_AT_SYMLINK_NOFOLLOW | LPR_LINUX_AT_EMPTY_PATH), wait_not_ready);
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

int64_t lpr_linux_faccessat(uint64_t dirfd, uint64_t path_raw, uint64_t mode, uint64_t flags)
{
    const char *path = (const char *)(uintptr_t)path_raw;
    uint64_t handle;
    int64_t status = lpr_dir_handle_for(dirfd, path, &handle);
    if (status) return status;
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_FACCESSAT2,
        .handle = handle, .mode = mode, .flags = flags};
    kb2_fs_response_t response;
    return lpr_fs_call(&request, path, 0, 0, 0, 0, 0, &response, -1, 0);
}


int64_t lpr_linux_open_metadata(uint64_t dirfd, uint64_t path_raw, uint64_t flags, unsigned *wait_not_ready __attribute__((unused)))
{
    uint64_t open_flags = LPR_LINUX_O_RDONLY;
    if ((flags & LPR_LINUX_AT_SYMLINK_NOFOLLOW) != 0) {
        open_flags |= LPR_LINUX_O_NOFOLLOW;
    }
    int64_t fd = lpr_linux_openat(dirfd, path_raw, open_flags, 0, wait_not_ready);
    if (fd == -LPR_LINUX_EISDIR) {
        fd = lpr_linux_openat(
            dirfd,
            path_raw,
            open_flags | LPR_LINUX_O_DIRECTORY,
            0, wait_not_ready);
    }
    return fd;
}

int64_t lpr_linux_fchmod(uint64_t fd, uint64_t mode)
{
    const int64_t binding_status = lpr_filed_fd_binding(fd);
    if (binding_status) return binding_status;
    const lpr_filed_backend_t *file = lpr_filed_backend(fd);
    if (!file) return -LPR_LINUX_EBADF;
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_FCHMODAT,
        .handle = file->handle, .mode = mode, .flags = LPR_LINUX_AT_EMPTY_PATH};
    kb2_fs_response_t response;
    return lpr_fs_call(&request, "", 0, 0, 0, 0, 0, &response, -1, 0);
}


int64_t lpr_linux_fchmodat(uint64_t dirfd, uint64_t path_raw, uint64_t mode, uint64_t flags)
{
    const char *path = (const char *)(uintptr_t)path_raw;
    uint64_t handle;
    const int64_t status = lpr_dir_handle_for(dirfd, path, &handle);
    if (status) return status;
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_FCHMODAT,
        .handle = handle, .mode = mode, .flags = flags};
    kb2_fs_response_t response;
    return lpr_fs_call(&request, path, 0, 0, 0, 0, 0, &response, -1, 0);
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

static int64_t fs_utimens(uint64_t handle, const char *path, uint64_t flags, uint64_t times_raw)
{
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_UTIMENSAT,
        .handle = handle, .flags = flags};
    kb2_fs_response_t response;
    /* Linux's two timespec64 values are serialized explicitly, including
     * UTIME_NOW/OMIT sentinels; clock/permission decisions belong to VFS. */
    uint8_t times[32];
    if (times_raw) {
        const lpr_linux_timespec_t *source = (const void *)(uintptr_t)times_raw;
        for (unsigned n = 0; n < 2; ++n)
            for (unsigned i = 0; i < 8; ++i) {
                times[n * 16 + i] = (uint8_t)((uint64_t)source[n].tv_sec >> (8 * i));
                times[n * 16 + 8 + i] = (uint8_t)((uint64_t)source[n].tv_nsec >> (8 * i));
            }
    }
    return lpr_fs_call(&request, path, 0, times_raw ? times : 0, times_raw ? sizeof(times) : 0,
        0, 0, &response, -1, 0);
}


int64_t lpr_filed_utimens_handle(uint64_t handle, uint64_t times_raw)
{
    return fs_utimens(handle, "", LPR_LINUX_AT_EMPTY_PATH, times_raw);
}

int64_t lpr_linux_utimensat(uint64_t dirfd, uint64_t path_raw, uint64_t times, uint64_t flags)
{
    /* Linux returns before looking up a pathname or validating flags when
     * neither timestamp changes. Preserve that syscall ordering even when
     * the supplied dirfd has no corresponding sandbox handle. */
    if (lpr_linux_utimens_both_omit((const void *)(uintptr_t)times)) return 0;
    const char *path = (const char *)(uintptr_t)path_raw;
    uint64_t handle;
    if (!path) {
        if (flags) return -LPR_LINUX_EINVAL;
        const int64_t binding_status = lpr_filed_fd_binding(dirfd);
        if (binding_status) return binding_status;
        const lpr_filed_backend_t *file = lpr_filed_backend(dirfd);
        if (!file) return -LPR_LINUX_EBADF;
        return fs_utimens(file->handle, "", LPR_LINUX_AT_EMPTY_PATH, times);
    }
    const int64_t status = lpr_dir_handle_for(dirfd, path, &handle);
    if (status) return status;
    return fs_utimens(handle, path, flags, times);
}

int64_t lpr_linux_readlink(uint64_t path, uint64_t buf, uint64_t bufsiz)
{
    if (bufsiz == 0) return -LPR_LINUX_EINVAL;
    if (buf == 0 && bufsiz != 0) {
        return -LPR_LINUX_EFAULT;
    }
    const char *path_string = (const char *)(uintptr_t)path;
    const char *trace_path = path_string;
    if (trace_path != 0) {
        pacha_trace2(
            PACHA_TRACE_COMPONENT_LPR,
            PACHA_TRACE_EVENT_LPR_PROCESS,
            PACHA_TRACE_CLASS_DEBUG,
            pacha_trace_name_id("readlink_path"),
            pacha_trace_name_id(trace_path));
    }
    char target[LPR_LINUX_SYMLINK_MAX];
    lpr_memset(target, 0, sizeof(target));
    const int64_t status = lpr_linux_readlinkat_to_buffer(
        (uint64_t)(int64_t)LPR_LINUX_AT_FDCWD,
        path,
        target,
        sizeof(target));
    if (status < 0) {
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
    if (!lpr_fd_is_filed(fd)) return -LPR_LINUX_EBADF;
    if (!buf && count) return -LPR_LINUX_EFAULT;
    const int64_t proc_status = lpr_linux_proc_getdents64(fd, buf, count);
    if (proc_status != -LPR_LINUX_ENOENT) return proc_status;
    const int64_t binding_status = lpr_filed_endpoint_ready();
    if (binding_status) return binding_status;
    const lpr_filed_backend_t *file = lpr_filed_backend(fd);
    if (!file) return -LPR_LINUX_EBADF;
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_GETDENTS64,
        .handle = file->handle, .length = count};
    kb2_fs_response_t response;
    return lpr_fs_call(&request, 0, 0, 0, 0, (void *)(uintptr_t)buf, count, &response, -1, 0);
}
