#include "../lpr_filed_internal.h"
#include "../lpr_gui_detail.h"
#include "../lpr_random.h"
#include "session.h"

int64_t lpr_filed_io_handle(
    uint32_t op, uint64_t handle, uint64_t buf, uint64_t count, uint64_t offset)
{
    if ((!buf && count) || (count && buf > UINT64_MAX - (count - 1u)))
        return -LPR_LINUX_EFAULT;
    if (count > 0x7ffff000u) count = 0x7ffff000u;
    const int writing = op == KB2_FILESYSTEM_OP_WRITE || op == KB2_FILESYSTEM_OP_PWRITE;
    kb2_fs_request_t request = {.opcode = op, .handle = handle,
        .offset = (int64_t)offset, .length = count};
    kb2_fs_response_t response;
    const int64_t result = lpr_fs_call(&request, 0, 0,
        writing ? (const void *)(uintptr_t)buf : 0, writing ? count : 0,
        writing ? 0 : (void *)(uintptr_t)buf, writing ? 0 : count,
        &response, -1, 0);
    if (result >= 0 && ((uint64_t)result > count ||
        (!writing && response.data_length != (uint64_t)result)))
        return -LPR_LINUX_EIO;
    return result;
}

int64_t lpr_filed_io(uint32_t op, uint64_t fd, uint64_t buf, uint64_t count, uint64_t offset)
{
    const int64_t binding_status = lpr_filed_fd_binding(fd);
    if (binding_status) return binding_status;
    const lpr_filed_backend_t *file = lpr_filed_backend(fd);
    return file ? lpr_filed_io_handle(op, file->handle, buf, count, offset) :
        -LPR_LINUX_EBADF;
}

int64_t lpr_fs_iov(uint64_t handle, uint64_t iov_raw, uint64_t iov_count, int writing)
{
    if (iov_count > 1024u) return -LPR_LINUX_EINVAL;
    if (!iov_raw && iov_count) return -LPR_LINUX_EFAULT;
    const lpr_linux_iovec_t *iov = (const void *)(uintptr_t)iov_raw;
    struct lpr_fs_buffer buffers[1024];
    uint64_t requested = 0;
    for (uint64_t i = 0; i < iov_count; ++i) {
        lpr_linux_iovec_t element;
        lpr_memcpy(&element, &iov[i], sizeof(element));
        if (element.len && (!element.base ||
            element.base > UINT64_MAX - (element.len - 1u)))
            return -LPR_LINUX_EFAULT;
        if (element.len > INT64_MAX - requested) return -LPR_LINUX_EINVAL;
        requested += element.len;
        buffers[i] = (struct lpr_fs_buffer){(const void *)(uintptr_t)element.base, element.len};
    }
    if (requested > 0x7ffff000u) requested = 0x7ffff000u;
    if (!requested) return lpr_filed_io_handle(writing ? KB2_FILESYSTEM_OP_WRITE :
        KB2_FILESYSTEM_OP_READ, handle, 0, 0, 0);
    /* Fixed-size private vector metadata follows Linux UIO_MAXIOV, not an
     * I/O-byte ceiling. Payload copy/scatter happens only after admission into
     * the charged arena; blocked callers do not allocate full-vector scratch. */
    unsigned parts = 0;
    uint64_t remaining = requested;
    for (uint64_t i = 0; i < iov_count && remaining; ++i) {
        uint64_t bytes = buffers[i].length;
        if (bytes > remaining) bytes = remaining;
        buffers[parts++].length = bytes;
        remaining -= bytes;
    }
    /* One Linux stream operation covers the entire vector, so a dup/fork
     * sibling cannot interleave a cursor advance between vector elements. */
    kb2_fs_request_t request = {.opcode = writing ? KB2_FILESYSTEM_OP_WRITE : KB2_FILESYSTEM_OP_READ,
        .handle = handle, .length = requested,
        .data = writing ? (kb2_fs_span_t){KB2_FILESYSTEM_REQUEST_SIZE, requested} : (kb2_fs_span_t){0}};
    kb2_fs_response_t response;
    const int64_t result = lpr_fs_call_buffers(&request, writing ? buffers : 0, writing ? parts : 0,
        &response, writing ? 0 : buffers, writing ? 0 : parts);
    if (result >= 0 && ((uint64_t)result > requested ||
        (!writing && response.data_length != (uint64_t)result))) return -LPR_LINUX_EIO;
    return result;
}

#if defined(LPR_ELF_READ_DIAG) && LPR_ELF_READ_DIAG
/* Diagnostic level 2 suppresses structurally normal loader reads. Serial
 * output for every library in every WebProcess distorts startup timings.
 * This is only a log filter: never change the result or retry a failed read. */
#if LPR_ELF_READ_DIAG > 1
static int lpr_elf_read_diag_normal(uint64_t buf, int64_t result)
{
    if (result != 960 || buf == 0) return 0;
    const unsigned char *b = (const unsigned char *)(uintptr_t)buf;
    if (b[0] != 0x7f || b[1] != 'E' || b[2] != 'L' || b[3] != 'F' ||
        b[4] != 2 || b[5] != 1 || (b[16] != 2 && b[16] != 3) || b[17])
        return 0;
    uint64_t phoff;
    lpr_memcpy(&phoff, b + 32, sizeof(phoff));
    const unsigned stride = b[54] | (unsigned)b[55] << 8;
    const unsigned count = b[56] | (unsigned)b[57] << 8;
    if (phoff > 960 || stride < 56 || count > (960 - phoff) / stride)
        return 0;
    for (unsigned i = 0; i < count; ++i) {
        const unsigned char *ph = b + phoff + (uint64_t)i * stride;
        uint32_t type;
        uint64_t address;
        lpr_memcpy(&type, ph, sizeof(type));
        lpr_memcpy(&address, ph + 16, sizeof(address));
        if (type == 2 && address != 0) return 1;
    }
    return 0;
}
#endif
static int64_t lpr_backend_read_impl(const lpr_fd_pin_t *pin, uint64_t buf, uint64_t count);
int64_t lpr_backend_read(const lpr_fd_pin_t *pin, uint64_t buf, uint64_t count)
{
    const int64_t result = lpr_backend_read_impl(pin, buf, count);
    if (pin && pin->ops_id == LPR_FD_OPS_FILED && count == 960) {
#if LPR_ELF_READ_DIAG > 1
        if (lpr_elf_read_diag_normal(buf, result)) return result;
#endif
        const lpr_filed_backend_t *file = pin->state;
        /* The native LOG syscall rejects (not truncates) messages > 256
         * bytes. Keep room for long library paths as well as the header. */
        char line[256];
        unsigned n = 0;
        const char *prefix = "[lpr-elf-read] ";
        while (*prefix) line[n++] = *prefix++;
        uint64_t values[] = {lpr_linux_current_pid, pin->fd, (uint64_t)result,
            file->handle, lpr_filed_control_offset(pin->fd)};
        for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
            for (int shift = 60; shift >= 0; shift -= 4)
                line[n++] = "0123456789abcdef"[(values[i] >> shift) & 15];
            line[n++] = ' ';
        }
        const unsigned char *bytes = (const unsigned char *)(uintptr_t)buf;
        for (unsigned i = 0; result > 0 && i < 32 && i < (uint64_t)result; ++i) {
            line[n++] = "0123456789abcdef"[bytes[i] >> 4];
            line[n++] = "0123456789abcdef"[bytes[i] & 15];
        }
        line[n++] = '\n';
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_LOG, (uint64_t)(uintptr_t)line, n);
    }
    return result;
}
static int64_t lpr_backend_read_impl(const lpr_fd_pin_t *pin, uint64_t buf, uint64_t count)
#else
int64_t lpr_backend_read(const lpr_fd_pin_t *pin, uint64_t buf, uint64_t count)
#endif
{
    if (pin == 0 || pin->state == 0) {
        return -LPR_LINUX_EBADF;
    }
    const uint64_t fd = pin->fd;
    switch (pin->ops_id) {
    case LPR_FD_OPS_INSTALL_DISK: {
        const uint64_t offset = lpr_filed_control_offset(fd);
        const int64_t n = lpr_block_io(fd, buf, count, offset, 0);
        if (n > 0) lpr_filed_control_advance_offset(fd, offset, (uint64_t)n);
        return n;
    }
    case LPR_FD_OPS_DEVICE: {
        const lpr_device_backend_t *device = pin->state;
        if ((device->flags & LPR_LINUX_O_ACCMODE) == LPR_LINUX_O_WRONLY) {
            return -LPR_LINUX_EBADF;
        }
        if (count > INT64_MAX) {
            return -LPR_LINUX_EINVAL;
        }
        if (device->major != 1 || device->minor == 3) {
            return 0;
        }
        if (count != 0 && buf == 0) {
            return -LPR_LINUX_EFAULT;
        }
        if (device->minor == 5) {
            lpr_memset((void *)(uintptr_t)buf, 0, (size_t)count);
            return (int64_t)count;
        }
        if (device->minor == 8 || device->minor == 9) {
            return lpr_linux_getrandom(buf, count, 0);
        }
        return 0;
    }
    case LPR_FD_OPS_INPUT:
        return lpr_input_read_events(fd, buf, count);
    case LPR_FD_OPS_DRM:
        return lpr_drm_read_events(fd, buf, count);
    case LPR_FD_OPS_TTY:
        return lpr_tty_io(TERMD_OP_HANDLE_READ, fd, buf, count);
    case LPR_FD_OPS_EVENT: {
        lpr_event_backend_t *event = pin->state;
        if (event->subtype == LPR_EVENT_BACKEND_INOTIFY) {
            return lpr_linux_inotify_read(fd, buf, count);
        }
        if (event->subtype == LPR_EVENT_BACKEND_SIGNALFD) {
            return lpr_linux_signalfd_read(fd, buf, count);
        }
        if (event->subtype == LPR_EVENT_BACKEND_TIMERFD) {
            return lpr_linux_timerfd_read(fd, buf, count);
        }
        if (event->subtype != LPR_EVENT_BACKEND_EVENTFD) {
            return -LPR_LINUX_EBADF;
        }
        if (count < sizeof(uint64_t)) {
            return -LPR_LINUX_EINVAL;
        }
        if (buf == 0) {
            return -LPR_LINUX_EFAULT;
        }
#if defined(LPR_GLYCIN_DIAG) && LPR_GLYCIN_DIAG
        if (__atomic_load_n(&lpr_glycin_diag_armed, __ATOMIC_ACQUIRE) != 0u) {
            lpr_glycin_diag_event(
                "event.read.enter", fd, event->counter,
                event->notify_pending, count);
        }
#endif
        uint64_t consumed = 0;
        for (;;) {
            uint64_t counter = __atomic_load_n(
                &event->counter, __ATOMIC_ACQUIRE);
            while (counter == 0) {
                if ((event->flags & LPR_LINUX_O_NONBLOCK) != 0)
                    return -LPR_LINUX_EAGAIN;
                lpr_wait_graph_t graph;
                lpr_wait_deadline_t deadline;
                lpr_wait_graph_init(&graph);
                int64_t wait_status = lpr_wait_graph_add_fd(
                    &graph, fd, 0x0001u);
                if (wait_status == 0)
                    wait_status = lpr_wait_deadline_init(&deadline, -1);
                if (wait_status == 0)
                    wait_status = lpr_wait_graph_block(&graph, &deadline);
                if (wait_status != 0) return wait_status;
                counter = __atomic_load_n(
                    &event->counter, __ATOMIC_ACQUIRE);
            }
            const uint64_t next =
                (event->reserved1 & LPR_LINUX_EFD_SEMAPHORE) != 0 ?
                    counter - 1u : 0u;
            if (__atomic_compare_exchange_n(
                    &event->counter,
                    &counter,
                    next,
                    0,
                    __ATOMIC_ACQ_REL,
                    __ATOMIC_ACQUIRE))
            {
                consumed =
                    (event->reserved1 & LPR_LINUX_EFD_SEMAPHORE) != 0 ?
                        1u : counter;
                break;
            }
        }
        *(uint64_t *)(uintptr_t)buf = consumed;
        lpr_event_backend_notify(event);
#if defined(LPR_GLYCIN_DIAG) && LPR_GLYCIN_DIAG
        if (__atomic_load_n(&lpr_glycin_diag_armed, __ATOMIC_ACQUIRE) != 0u) {
            lpr_glycin_diag_event(
                "event.read.exit", fd, event->counter,
                event->notify_pending,
                (int64_t)consumed);
        }
#endif
        return (int64_t)sizeof(uint64_t);
    }
    case LPR_FD_OPS_PIPE: {
        lpr_pipe_backend_t *pipe = pin->state;
        if (!pipe->readable || pipe->native.raw < 0) {
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
                PACHAOS_SYSCALL_FD_READ,
                (uint64_t)(uint32_t)pipe->native.raw,
                buf,
                count);
            if (n >= 0) return n;
            const int64_t err = lpr_pacha_status_to_errno(n);
            if (err != -LPR_LINUX_EAGAIN ||
                (pipe->flags & LPR_LINUX_O_NONBLOCK) != 0)
            {
                return err;
            }
            const int64_t wait_status = lpr_pipe_wait(fd, 0x0001u, 0);
            if (wait_status != 0) {
                return wait_status;
            }
        }
    }
    case LPR_FD_OPS_FILED: {
        const lpr_filed_backend_t *file = pin->state;
        const int64_t binding_status = lpr_filed_endpoint_ready();
        if (binding_status) return binding_status;
        /* READ, including readonly descriptors, advances Linux's shared
         * struct file cursor. A personality-owned shadow cursor breaks dup
         * and fork and cannot observe other clients' reads. */
        return lpr_filed_io_handle(KB2_FILESYSTEM_OP_READ, file->handle, buf, count, 0);
    }
    default:
        return -LPR_LINUX_EBADF;
    }
}

int64_t lpr_backend_readv(
    const lpr_fd_pin_t *pin,
    uint64_t iov_raw,
    uint64_t iov_count)
{
    if (pin == 0 || pin->state == 0) {
        return -LPR_LINUX_EBADF;
    }
    if (iov_raw == 0 && iov_count != 0) {
        return -LPR_LINUX_EFAULT;
    }
    const uint64_t fd = pin->fd;
    switch (pin->ops_id) {
    case LPR_FD_OPS_INSTALL_DISK:
        return lpr_iov_scalar_io(fd, iov_raw, iov_count, 0);
    case LPR_FD_OPS_DEVICE:
        return lpr_iov_scalar_io(fd, iov_raw, iov_count, 0);
    case LPR_FD_OPS_PIPE: {
        lpr_pipe_backend_t *pipe = (lpr_pipe_backend_t *)pin->state;
        if (!pipe->readable || pipe->native.raw < 0) {
            return -LPR_LINUX_EBADF;
        }
        for (;;) {
            const int64_t n = lpr_pacha_syscall3(
                PACHAOS_SYSCALL_FD_READV,
                (uint64_t)(uint32_t)pipe->native.raw,
                iov_raw,
                iov_count);
            if (n >= 0) {
                return n;
            }
            const int64_t err = lpr_pacha_status_to_errno(n);
            if (err != -LPR_LINUX_EAGAIN ||
                (pipe->flags & LPR_LINUX_O_NONBLOCK) != 0)
            {
                return err;
            }
            const int64_t wait_status = lpr_pipe_wait(fd, 0x0001u, 0);
            if (wait_status != 0) {
                return wait_status;
            }
        }
    }
    case LPR_FD_OPS_TTY:
        return lpr_iov_scalar_io(fd, iov_raw, iov_count, 0);
    case LPR_FD_OPS_DRM:
        return lpr_iov_scalar_io(fd, iov_raw, iov_count, 0);
    case LPR_FD_OPS_INPUT:
        return lpr_iov_scalar_io(fd, iov_raw, iov_count, 0);
    case LPR_FD_OPS_EVENT: {
        const lpr_event_backend_t *event =
            (const lpr_event_backend_t *)pin->state;
        if (event->subtype != LPR_EVENT_BACKEND_EVENTFD &&
            (event->subtype != LPR_EVENT_BACKEND_TIMERFD || !event->active) &&
            (event->subtype != LPR_EVENT_BACKEND_SIGNALFD || !event->active) &&
            (event->subtype != LPR_EVENT_BACKEND_INOTIFY || !event->active))
        {
            return -LPR_LINUX_EBADF;
        }
        int64_t total = 0;
        const lpr_linux_iovec_t *iov = (const lpr_linux_iovec_t *)(uintptr_t)iov_raw;
        for (uint64_t i = 0; i < iov_count; i += 1) {
            const int64_t n = lpr_backend_read(pin, iov[i].base, iov[i].len);
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
    case LPR_FD_OPS_FILED:
        break;
    default:
        return -LPR_LINUX_EBADF;
    }
    const lpr_filed_backend_t *file = pin->state;
    const int64_t binding_status = lpr_filed_endpoint_ready();
    if (binding_status) return binding_status;
    return lpr_fs_iov(file->handle, iov_raw, iov_count, 0);
}

int64_t lpr_linux_pread64(uint64_t fd, uint64_t buf, uint64_t count, uint64_t offset)
{
    if (lpr_block_backend(fd))
        return lpr_block_io(fd, buf, count, offset, 0);
    const int64_t binding_status = lpr_filed_fd_binding(fd);
    if (binding_status) return binding_status;
    const lpr_filed_backend_t *file = lpr_filed_backend(fd);
    if (file == 0) {
        return -LPR_LINUX_EBADF;
    }
    return lpr_filed_io_handle(
        KB2_FILESYSTEM_OP_PREAD, file->handle, buf, count, offset);
}

int64_t lpr_linux_pwrite64(uint64_t fd, uint64_t buf, uint64_t count, uint64_t offset)
{
    if (lpr_block_backend(fd))
        return lpr_block_io(fd, buf, count, offset, 1);
    if ((int64_t)offset < 0) return -LPR_LINUX_EINVAL;
    if (fd > LPR_LINUX_FD_MAX) return -LPR_LINUX_EBADF;
    lpr_fd_pin_t pin;
    if (lpr_fd_table_pin(&lpr_control_fd_table, (uint32_t)fd, &pin) != 0)
        return -LPR_LINUX_EBADF;
    int64_t result;
    if ((pin.effective_rights & LPR_FD_RIGHT_WRITE) == 0) {
        result = -LPR_LINUX_EBADF;
    } else if (pin.ops_id != LPR_FD_OPS_FILED) {
        result = -LPR_LINUX_ESPIPE;
    } else if ((result = lpr_filed_endpoint_ready()) != 0) {
        /* Keep the pin alive while adoption updates its backend in place. */
    } else {
        const lpr_filed_backend_t *file = pin.state;
        if (lpr_memfd_write_is_sealed(file->reserved1)) {
            result = -LPR_LINUX_EPERM;
        } else if ((buf == 0 && count != 0) ||
                   (count != 0 && buf > UINT64_MAX - (count - 1u))) {
            result = -LPR_LINUX_EFAULT;
        } else {
            /* O_APPEND handling, sealing and positional-write limits are
             * Linux file semantics; never invent an append sentinel. */
            result = lpr_filed_io_handle(KB2_FILESYSTEM_OP_PWRITE,
                file->handle, buf, count, offset);
        }
    }
    lpr_fd_unpin(&pin);
    return result;
}

int64_t lpr_linux_pread_to_vmo(
    uint64_t fd, uint64_t vmo_fd, uint64_t vmo_offset,
    uint64_t count, uint64_t file_offset)
{
    if (vmo_fd < 16 || vmo_offset > UINT64_MAX - count)
        return -LPR_LINUX_EINVAL;
    if (!count) return 0;
    const uint64_t base = vmo_offset & ~4095ull;
    const uint64_t prefix = vmo_offset - base;
    const uint64_t mapped_bytes = lpr_page_align_up(prefix + count);
    if (!mapped_bytes) return -LPR_LINUX_EOVERFLOW;
    const int64_t mapped = lpr_pacha_syscall6(PACHAOS_SYSCALL_MMAP,
        vmo_fd, 0, mapped_bytes, PACHAOS_PROT_READ | PACHAOS_PROT_WRITE,
        PACHAOS_MMAP_SHARED, base);
    if (mapped < 4096) return lpr_pacha_status_to_errno(mapped);
    const int64_t result = lpr_linux_pread64(fd, mapped + prefix, count, file_offset);
    (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, mapped, mapped_bytes);
    return result;
}
