#include "../lpr_filed_internal.h"

int lpr_pipe_fd_is_active(uint64_t fd)
{
    return lpr_fd_pipe_payload(fd) != 0;
}

int lpr_linux_pipe_fd_active(uint64_t fd)
{
    return lpr_pipe_fd_is_active(fd);
}

uint64_t lpr_pipe_writev_wait_min(uint64_t iov_raw, uint64_t iov_count)
{
    if (iov_raw == 0 || iov_count == 0) {
        return 1;
    }
    const lpr_linux_iovec_t *iov = (const lpr_linux_iovec_t *)(uintptr_t)iov_raw;
    uint64_t total = 0;
    for (uint64_t i = 0; i < iov_count; i += 1) {
        if (iov[i].len > LPR_LINUX_PIPE_BUF_BYTES || total > LPR_LINUX_PIPE_BUF_BYTES - iov[i].len) {
            return 1;
        }
        total += iov[i].len;
    }
    return total == 0 ? 1 : total;
}

int lpr_native_pipe_fd_info(uint64_t fd, struct pacha_fd_info *out)
{
    if (fd > LPR_LINUX_FD_MAX || out == 0) {
        return 0;
    }
    if (lpr_fd_local_active(fd) || lpr_linux_socket_fd_active(fd)) {
        return 0;
    }
    return lpr_native_fd_info(fd, out) && out->kind == PACHA_FD_KIND_PIPE;
}

int lpr_native_pipe_slot_claimable(uint64_t fd, struct pacha_fd_info *out)
{
    if (fd > LPR_LINUX_FD_MAX ||
        out == 0 ||
        lpr_runtime_reserved_fd(fd) ||
        lpr_fd_local_active(fd) ||
        lpr_linux_socket_fd_active(fd))
    {
        return 0;
    }
    return lpr_native_fd_info(fd, out) && out->kind == PACHA_FD_KIND_PIPE;
}

uint64_t lpr_pipe_flags_to_pacha(uint64_t flags)
{
    uint64_t out = 0;
    if ((flags & LPR_LINUX_O_CLOEXEC) != 0) {
        out |= PACHA_FD_FLAG_CLOEXEC;
    }
    if ((flags & LPR_LINUX_O_NONBLOCK) != 0) {
        out |= PACHA_FD_FLAG_NONBLOCK;
    }
    return out;
}

uint64_t lpr_pipe_rights(int readable)
{
    return PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_DUP |
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL |
        PACHA_FD_RIGHT_SET_FLAGS |
        PACHA_FD_RIGHT_CLOSE |
        (readable ? PACHA_FD_RIGHT_READ : PACHA_FD_RIGHT_WRITE);
}

uint64_t lpr_pipe_poll_events_to_pacha(uint32_t events)
{
    uint64_t out = 0;
    if ((events & 0x0001u) != 0) {
        out |= PACHA_FD_EVENT_READABLE;
    }
    if ((events & 0x0004u) != 0) {
        out |= PACHA_FD_EVENT_WRITABLE;
    }
    if ((events & 0x0008u) != 0) {
        out |= PACHA_FD_EVENT_ERROR;
    }
    return out;
}

uint32_t lpr_pipe_poll_events_from_pacha(uint64_t events)
{
    uint32_t out = 0;
    if ((events & PACHA_FD_EVENT_READABLE) != 0) {
        out |= 0x0001u;
    }
    if ((events & PACHA_FD_EVENT_WRITABLE) != 0) {
        out |= 0x0004u;
    }
    if ((events & PACHA_FD_EVENT_ERROR) != 0) {
        out |= 0x0008u;
    }
    if ((events & PACHA_FD_EVENT_HANGUP) != 0) {
        out |= 0x0010u;
    }
    return out;
}

uint32_t lpr_linux_pipe_poll_events(uint64_t fd, uint32_t events)
{
    if (!lpr_pipe_fd_is_active(fd)) {
        return 0x0020u;
    }
    struct pacha_pollfd pollfd;
    lpr_memset(&pollfd, 0, sizeof(pollfd));
    pollfd.fd = (int)(uint32_t)fd;
    pollfd.events = lpr_pipe_poll_events_to_pacha(events);
    const int64_t status = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_FD_POLL,
        (uint64_t)(uintptr_t)&pollfd,
        1);
    if (status != 0 && pollfd.revents == 0) {
        return 0x0020u;
    }
    return lpr_pipe_poll_events_from_pacha(pollfd.revents);
}

uint32_t lpr_linux_native_fd_poll_events(uint64_t fd, uint32_t events)
{
    struct pacha_fd_info info;
    if (!lpr_native_pipe_fd_info(fd, &info)) {
        return 0x0020u;
    }
    struct pacha_pollfd pollfd;
    lpr_memset(&pollfd, 0, sizeof(pollfd));
    pollfd.fd = (int)(uint32_t)fd;
    pollfd.events = lpr_pipe_poll_events_to_pacha(events);
    const int64_t status = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_FD_POLL,
        (uint64_t)(uintptr_t)&pollfd,
        1);
    if (status != 0 && pollfd.revents == 0) {
        return 0x0020u;
    }
    return lpr_pipe_poll_events_from_pacha(pollfd.revents);
}

int64_t lpr_pipe_wait(uint64_t fd, uint32_t events, uint64_t min_write_bytes)
{
    for (;;) {
        struct pacha_pollfd pollfd;
        lpr_memset(&pollfd, 0, sizeof(pollfd));
        pollfd.fd = (int)(uint32_t)fd;
        pollfd.events = lpr_pipe_poll_events_to_pacha(events | 0x0008u);
        pollfd.revents = ((events & 0x0004u) != 0) ? min_write_bytes : 0;
        const int64_t wait_status = lpr_pacha_syscall4(
            PACHAOS_SYSCALL_FD_WAIT_MANY,
            (uint64_t)(uintptr_t)&pollfd,
            1,
            PACHA_FD_WAIT_FOREVER,
            0);
        if (wait_status == PACHA_SYSCALL_ERR_NOT_READY ||
            wait_status == -PACHA_SYSCALL_ERR_NOT_READY)
        {
            continue;
        }
        if (wait_status < 0) {
            const int64_t errno_status = lpr_pacha_status_to_errno(wait_status);
            if (errno_status == -LPR_LINUX_EAGAIN) {
                continue;
            }
            return errno_status;
        }
        if (wait_status > 0) {
            const uint32_t revents = lpr_pipe_poll_events_from_pacha(pollfd.revents);
            if ((revents & 0x0008u) != 0) {
                lpr_linux_raise_sigpipe();
                return -LPR_LINUX_EPIPE;
            }
            if ((revents & (events | 0x0010u)) != 0) {
                return 0;
            }
        }
    }
}

int64_t lpr_native_pipe_read(uint64_t fd, uint64_t buf, uint64_t count)
{
    struct pacha_fd_info info;
    if (!lpr_native_pipe_fd_info(fd, &info)) {
        return -LPR_LINUX_EBADF;
    }
    if ((info.rights & PACHA_FD_RIGHT_READ) == 0) {
        return -LPR_LINUX_EBADF;
    }
    if (count == 0) {
        return 0;
    }
    if (buf == 0) {
        return -LPR_LINUX_EFAULT;
    }
    for (;;) {
        const int64_t n = lpr_pacha_syscall3(PACHAOS_SYSCALL_FD_READ, fd, buf, count);
        if (n >= 0) {
            return n;
        }
        const int64_t err = lpr_pacha_status_to_errno(n);
        if (err != -LPR_LINUX_EAGAIN ||
            (info.flags & PACHA_FD_FLAG_NONBLOCK) != 0)
        {
            return err;
        }
        const int64_t wait_status = lpr_pipe_wait(fd, 0x0001u, 0);
        if (wait_status != 0) {
            return wait_status;
        }
    }
}

int64_t lpr_native_pipe_readv(uint64_t fd, uint64_t iov_raw, uint64_t iov_count)
{
    struct pacha_fd_info info;
    if (!lpr_native_pipe_fd_info(fd, &info)) {
        return -LPR_LINUX_EBADF;
    }
    if ((info.rights & PACHA_FD_RIGHT_READ) == 0) {
        return -LPR_LINUX_EBADF;
    }
    for (;;) {
        const int64_t n = lpr_pacha_syscall3(PACHAOS_SYSCALL_FD_READV, fd, iov_raw, iov_count);
        if (n >= 0) {
            return n;
        }
        const int64_t err = lpr_pacha_status_to_errno(n);
        if (err != -LPR_LINUX_EAGAIN ||
            (info.flags & PACHA_FD_FLAG_NONBLOCK) != 0)
        {
            return err;
        }
        const int64_t wait_status = lpr_pipe_wait(fd, 0x0001u, 0);
        if (wait_status != 0) {
            return wait_status;
        }
    }
}

int64_t lpr_native_pipe_write(uint64_t fd, uint64_t buf, uint64_t count)
{
    struct pacha_fd_info info;
    if (!lpr_native_pipe_fd_info(fd, &info)) {
        return -LPR_LINUX_EBADF;
    }
    if ((info.rights & PACHA_FD_RIGHT_WRITE) == 0) {
        return -LPR_LINUX_EBADF;
    }
    if (count == 0) {
        return 0;
    }
    if (buf == 0) {
        return -LPR_LINUX_EFAULT;
    }
    for (;;) {
        const int64_t n = lpr_pacha_syscall3(PACHAOS_SYSCALL_FD_WRITE, fd, buf, count);
        if (n >= 0) {
            return n;
        }
        const int64_t err = lpr_pacha_status_to_errno(n);
        if (err == -LPR_LINUX_EPIPE) {
            lpr_linux_raise_sigpipe();
            return err;
        }
        if (err != -LPR_LINUX_EAGAIN ||
            (info.flags & PACHA_FD_FLAG_NONBLOCK) != 0)
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

int64_t lpr_native_pipe_writev(uint64_t fd, uint64_t iov_raw, uint64_t iov_count)
{
    struct pacha_fd_info info;
    if (!lpr_native_pipe_fd_info(fd, &info)) {
        return -LPR_LINUX_EBADF;
    }
    if ((info.rights & PACHA_FD_RIGHT_WRITE) == 0) {
        return -LPR_LINUX_EBADF;
    }
    for (;;) {
        const int64_t n = lpr_pacha_syscall3(PACHAOS_SYSCALL_FD_WRITEV, fd, iov_raw, iov_count);
        if (n >= 0) {
            return n;
        }
        const int64_t err = lpr_pacha_status_to_errno(n);
        if (err == -LPR_LINUX_EPIPE) {
            lpr_linux_raise_sigpipe();
            return err;
        }
        if (err != -LPR_LINUX_EAGAIN ||
            (info.flags & PACHA_FD_FLAG_NONBLOCK) != 0)
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

int lpr_linux_eventfd_active(uint64_t fd)
{
    return lpr_fd_event_payload(fd) != 0;
}
