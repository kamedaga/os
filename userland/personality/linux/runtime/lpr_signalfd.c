#include "lpr_filed_internal.h"

#define LPR_SIGNALFD_CLOEXEC 02000000ull
#define LPR_SIGNALFD_NONBLOCK 00004000ull
#define LPR_SIGNALFD_SIGINFO_BYTES 128u
#define LPR_SIGNALFD_POLLIN 0x0001u

typedef struct lpr_linux_signalfd_siginfo {
    uint32_t signo;
    int32_t error;
    int32_t code;
    uint32_t pid;
    uint32_t uid;
    int32_t fd;
    uint32_t tid;
    uint32_t band;
    uint32_t overrun;
    uint32_t trapno;
    int32_t status;
    int32_t integer;
    uint64_t pointer;
    uint64_t user_time;
    uint64_t system_time;
    uint64_t address;
    uint16_t address_lsb;
    uint16_t pad0;
    int32_t syscall;
    uint64_t call_address;
    uint32_t architecture;
    uint8_t pad[28];
} lpr_linux_signalfd_siginfo_t;

_Static_assert(
    sizeof(lpr_linux_signalfd_siginfo_t) == LPR_SIGNALFD_SIGINFO_BYTES,
    "Linux signalfd_siginfo layout");

static uint64_t lpr_signalfd_backend_mask(const lpr_event_backend_t *event)
{
    if (event == 0 || !event->active ||
        event->subtype != LPR_EVENT_BACKEND_SIGNALFD)
    {
        return 0;
    }
    return event->counter & ~lpr_linux_unblockable_signal_mask();
}

int lpr_linux_signalfd_active(uint64_t fd)
{
    const lpr_event_backend_t *event = lpr_event_backend(fd);
    return event != 0 && event->active &&
        event->subtype == LPR_EVENT_BACKEND_SIGNALFD;
}

uint64_t lpr_signalfd_union_mask(void)
{
    return __atomic_load_n(
        &lpr_state.signal.signalfd_mask_union,
        __ATOMIC_ACQUIRE);
}

void lpr_signalfd_refresh_mask(void)
{
    uint64_t mask = 0;
    for (uint64_t fd = 0; fd < lpr_fd_table_capacity; ++fd) {
        mask |= lpr_signalfd_backend_mask(lpr_event_backend(fd));
    }
    __atomic_store_n(
        &lpr_state.signal.signalfd_mask_union,
        mask,
        __ATOMIC_RELEASE);
}

static uint64_t lpr_signalfd_ready_mask(const lpr_event_backend_t *event)
{
    return __atomic_load_n(
        &lpr_state.signal.signalfd_pending_mask,
        __ATOMIC_ACQUIRE) & lpr_signalfd_backend_mask(event);
}

static void lpr_signalfd_notify_matching(uint64_t signal_bit)
{
    for (uint64_t fd = 0; fd < lpr_fd_table_capacity; ++fd) {
        lpr_event_backend_t *event = lpr_event_backend(fd);
        if ((lpr_signalfd_backend_mask(event) & signal_bit) != 0) {
            lpr_event_backend_notify(event);
        }
    }
}

int lpr_signalfd_enqueue(uint32_t sig)
{
    const uint64_t bit = lpr_linux_signal_bit(sig);
    if (bit == 0 || (lpr_signalfd_union_mask() & bit) == 0) {
        return 0;
    }
    int matched = 0;
    for (uint64_t fd = 0; fd < lpr_fd_table_capacity; ++fd) {
        if ((lpr_signalfd_backend_mask(lpr_event_backend(fd)) & bit) != 0) {
            matched = 1;
            break;
        }
    }
    if (!matched) {
        return 0;
    }
    (void)__atomic_fetch_or(
        &lpr_state.signal.signalfd_pending_mask,
        bit,
        __ATOMIC_ACQ_REL);
    lpr_signalfd_notify_matching(bit);
    return 1;
}

int64_t lpr_linux_signalfd4(
    uint64_t fd_raw,
    uint64_t mask_raw,
    uint64_t sigsetsize,
    uint64_t flags)
{
    const uint64_t known_flags =
        LPR_SIGNALFD_CLOEXEC | LPR_SIGNALFD_NONBLOCK;
    if (mask_raw == 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (sigsetsize != sizeof(uint64_t) || (flags & ~known_flags) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    const uint64_t mask =
        *(const uint64_t *)(uintptr_t)mask_raw &
        ~lpr_linux_unblockable_signal_mask();
    const int64_t requested_fd = (int64_t)fd_raw;
    if (requested_fd != -1) {
        if (requested_fd < 0 ||
            !lpr_linux_signalfd_active((uint64_t)requested_fd))
        {
            return -LPR_LINUX_EINVAL;
        }
        lpr_event_backend_t *event =
            lpr_event_backend((uint64_t)requested_fd);
        const uint64_t old_mask = event->counter;
        event->counter = mask;
        lpr_signalfd_refresh_mask();
        const int64_t status = lpr_linux_sync_native_signal_mask();
        if (status != 0) {
            event->counter = old_mask;
            lpr_signalfd_refresh_mask();
            (void)lpr_linux_sync_native_signal_mask();
            return status;
        }
        if (lpr_signalfd_ready_mask(event) != 0) {
            lpr_event_backend_notify(event);
        }
        return requested_fd;
    }

    const int fd = lpr_fd_slot_alloc();
    if (fd < 0) {
        return fd;
    }
    int wait_fd = -1;
    int notify_fd = -1;
    const int pair_status = lpr_native_wait_pair(&wait_fd, &notify_fd);
    if (pair_status != 0) {
        return pair_status;
    }
    const int install_status = lpr_control_install_fd(
        (uint64_t)(uint32_t)fd,
        LPR_FD_OPS_EVENT,
        flags,
        0,
        mask);
    if (install_status != 0) {
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)wait_fd);
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)notify_fd);
        return install_status;
    }
    lpr_event_backend_t *event =
        lpr_event_backend((uint64_t)(uint32_t)fd);
    if (event == 0) {
        lpr_control_close_fd((uint64_t)(uint32_t)fd);
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)wait_fd);
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)notify_fd);
        return -LPR_LINUX_EIO;
    }
    event->active = 1;
    event->subtype = LPR_EVENT_BACKEND_SIGNALFD;
    event->flags = (uint32_t)flags;
    event->counter = mask;
    event->wait_fd.raw = wait_fd;
    event->notify_fd.raw = notify_fd;
    lpr_signalfd_refresh_mask();
    const int64_t sync_status = lpr_linux_sync_native_signal_mask();
    if (sync_status != 0) {
        lpr_control_close_fd((uint64_t)(uint32_t)fd);
        return sync_status;
    }
    if (lpr_signalfd_ready_mask(event) != 0) {
        lpr_event_backend_notify(event);
    }
    return fd;
}

static uint32_t lpr_signalfd_take_signal(lpr_event_backend_t *event)
{
    for (;;) {
        const uint64_t pending = __atomic_load_n(
            &lpr_state.signal.signalfd_pending_mask,
            __ATOMIC_ACQUIRE);
        const uint64_t ready = pending & lpr_signalfd_backend_mask(event);
        if (ready == 0) {
            return 0;
        }
        const uint32_t sig = (uint32_t)__builtin_ctzll(ready) + 1u;
        const uint64_t desired = pending & ~lpr_linux_signal_bit(sig);
        uint64_t expected = pending;
        if (__atomic_compare_exchange_n(
                &lpr_state.signal.signalfd_pending_mask,
                &expected,
                desired,
                0,
                __ATOMIC_ACQ_REL,
                __ATOMIC_ACQUIRE))
        {
            return sig;
        }
    }
}

int64_t lpr_linux_signalfd_read(uint64_t fd, uint64_t buf, uint64_t count)
{
    lpr_event_backend_t *event = lpr_event_backend(fd);
    if (!lpr_linux_signalfd_active(fd)) {
        return -LPR_LINUX_EBADF;
    }
    if (count < LPR_SIGNALFD_SIGINFO_BYTES) {
        return -LPR_LINUX_EINVAL;
    }
    if (buf == 0) {
        return -LPR_LINUX_EFAULT;
    }
    uint64_t written = 0;
    for (;;) {
        const uint32_t sig = lpr_signalfd_take_signal(event);
        if (sig != 0) {
            lpr_linux_signalfd_siginfo_t *info =
                (lpr_linux_signalfd_siginfo_t *)(uintptr_t)(buf + written);
            lpr_memset(info, 0, sizeof(*info));
            info->signo = sig;
            written += sizeof(*info);
            if (count - written < sizeof(*info)) {
                break;
            }
            continue;
        }
        if (written != 0) {
            break;
        }
        if ((event->flags & LPR_SIGNALFD_NONBLOCK) != 0) {
            return -LPR_LINUX_EAGAIN;
        }
        lpr_wait_graph_t graph;
        lpr_wait_deadline_t deadline;
        lpr_wait_graph_init(&graph);
        int64_t status = lpr_wait_graph_add_fd(
            &graph, fd, LPR_SIGNALFD_POLLIN);
        if (status == 0) {
            status = lpr_wait_deadline_init(&deadline, -1);
        }
        if (status == 0) {
            status = lpr_wait_graph_block(&graph, &deadline);
        }
        if (status != 0 && lpr_signalfd_ready_mask(event) == 0) {
            return status;
        }
    }
    lpr_eventfd_drain_wait(fd);
    if (lpr_signalfd_ready_mask(event) != 0) {
        lpr_event_backend_notify(event);
    }
    return (int64_t)written;
}

uint32_t lpr_linux_signalfd_poll_events(uint64_t fd, uint32_t events)
{
    lpr_event_backend_t *event = lpr_event_backend(fd);
    if (!lpr_linux_signalfd_active(fd)) {
        return 0;
    }
    if (lpr_signalfd_ready_mask(event) == 0) {
        return 0;
    }
    lpr_event_backend_notify(event);
    return events & LPR_SIGNALFD_POLLIN;
}
