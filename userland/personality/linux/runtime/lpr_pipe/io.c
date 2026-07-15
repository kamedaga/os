#include "../lpr_filed_internal.h"

int lpr_pipe_fd_is_active(uint64_t fd)
{
    return lpr_pipe_backend(fd) != 0;
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
    for (uint64_t i = 0; i < iov_count; i++) {
        if (iov[i].len > LPR_LINUX_PIPE_BUF_BYTES ||
            total > LPR_LINUX_PIPE_BUF_BYTES - iov[i].len)
        {
            return 1;
        }
        total += iov[i].len;
    }
    return total == 0 ? 1 : total;
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
    if ((events & 0x0001u) != 0) out |= PACHA_FD_EVENT_READABLE;
    if ((events & 0x0004u) != 0) out |= PACHA_FD_EVENT_WRITABLE;
    if ((events & 0x0008u) != 0) out |= PACHA_FD_EVENT_ERROR;
    return out;
}

uint32_t lpr_pipe_poll_events_from_pacha(uint64_t events)
{
    uint32_t out = 0;
    if ((events & PACHA_FD_EVENT_READABLE) != 0) out |= 0x0001u;
    if ((events & PACHA_FD_EVENT_WRITABLE) != 0) out |= 0x0004u;
    if ((events & PACHA_FD_EVENT_ERROR) != 0) out |= 0x0008u;
    if ((events & PACHA_FD_EVENT_HANGUP) != 0) out |= 0x0010u;
    return out;
}

uint32_t lpr_linux_pipe_poll_events(uint64_t fd, uint32_t events)
{
    const lpr_pipe_backend_t *pipe = lpr_pipe_backend(fd);
    if (pipe == 0 || pipe->native.raw < 0) {
        return 0x0020u;
    }
    struct pacha_pollfd pollfd;
    lpr_memset(&pollfd, 0, sizeof(pollfd));
    pollfd.fd = pipe->native.raw;
    pollfd.events = lpr_pipe_poll_events_to_pacha(events);
    const int64_t status = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_FD_POLL,
        (uint64_t)(uintptr_t)&pollfd,
        1);
    return status != 0 && pollfd.revents == 0 ?
        0x0020u : lpr_pipe_poll_events_from_pacha(pollfd.revents);
}

int64_t lpr_pipe_wait(uint64_t fd, uint32_t events, uint64_t min_write_bytes)
{
    const lpr_pipe_backend_t *pipe = lpr_pipe_backend(fd);
    if (pipe == 0 || pipe->native.raw < 0) {
        return -LPR_LINUX_EBADF;
    }
    for (;;) {
        struct pacha_pollfd pollfd;
        lpr_memset(&pollfd, 0, sizeof(pollfd));
        pollfd.fd = pipe->native.raw;
        pollfd.events = lpr_pipe_poll_events_to_pacha(events | 0x0008u);
        pollfd.revents = (events & 0x0004u) != 0 ? min_write_bytes : 0;
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
            const int64_t status = lpr_pacha_status_to_errno(wait_status);
            if (status == -LPR_LINUX_EAGAIN) continue;
            return status;
        }
        if (wait_status > 0) {
            const uint32_t revents = lpr_pipe_poll_events_from_pacha(pollfd.revents);
            if ((revents & 0x0008u) != 0) {
                lpr_linux_raise_sigpipe();
                return -LPR_LINUX_EPIPE;
            }
            if ((revents & (events | 0x0010u)) != 0) return 0;
        }
    }
}

int lpr_linux_eventfd_active(uint64_t fd)
{
    const lpr_event_backend_t *event = lpr_event_backend(fd);
    return event != 0 && event->subtype == LPR_EVENT_BACKEND_EVENTFD;
}
