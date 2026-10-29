#include "../lpr_filed_internal.h"

static uint64_t counter_rights(void)
{
    return PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
}

int lpr_event_counter_map(lpr_event_backend_t *event)
{
    if (event->counter_map) return 0;
    struct pacha_fd_info info = {0};
    if (event->counter_fd.raw < 16 ||
        !lpr_native_fd_info((uint64_t)(uint32_t)event->counter_fd.raw, &info) ||
        info.kind != PACHA_FD_KIND_VMO || info.size != LPR_EVENT_COUNTER_BYTES ||
        info.rights != counter_rights())
        return -LPR_LINUX_EBADF;
    const int64_t mapping = lpr_pacha_syscall6(PACHAOS_SYSCALL_MMAP,
        (uint64_t)(uint32_t)event->counter_fd.raw, 0, LPR_EVENT_COUNTER_BYTES,
        PACHAOS_PROT_READ | PACHAOS_PROT_WRITE, PACHAOS_MMAP_SHARED, 0);
    if (mapping < 4096) return -LPR_LINUX_ENOMEM;
    event->counter_map = (uint64_t)mapping;
    return 0;
}

int64_t lpr_event_counter_unmap(lpr_event_backend_t *event)
{
    int64_t status = 0;
    if (event->counter_map)
        status = lpr_pacha_status_to_errno(lpr_pacha_syscall2(
            PACHAOS_SYSCALL_MUNMAP, event->counter_map, LPR_EVENT_COUNTER_BYTES));
    event->counter_map = 0;
    return status;
}

int64_t lpr_event_counter_close(lpr_event_backend_t *event)
{
    int64_t status = lpr_event_counter_unmap(event);
    if (event->counter_fd.raw >= 16) {
        const int64_t closed = lpr_close_native_fd_if_open(
            (uint64_t)(uint32_t)event->counter_fd.raw);
        if (!status) status = closed;
    }
    event->counter_fd.raw = -1;
    return status;
}

int lpr_event_counter_create(lpr_event_backend_t *event, uint64_t initial)
{
    const int64_t fd = lpr_pacha_syscall3(PACHAOS_SYSCALL_VMO_CREATE,
        LPR_EVENT_COUNTER_BYTES, counter_rights(), 0);
    if (fd < 16)
        return fd ? (int)lpr_pacha_status_to_errno(fd) : -LPR_LINUX_EIO;
    event->counter_fd.raw = (pacha_fd_t)fd;
    const int status = lpr_event_counter_map(event);
    if (status) {
        (void)lpr_event_counter_close(event);
        return status;
    }
    __atomic_store_n(lpr_event_counter(event), initial, __ATOMIC_RELEASE);
    return 0;
}
