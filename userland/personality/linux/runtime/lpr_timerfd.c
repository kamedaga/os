#include "lpr_filed_internal.h"

#define LPR_TIMERFD_CLOEXEC 02000000ull
#define LPR_TIMERFD_NONBLOCK 00004000ull

typedef struct lpr_linux_itimerspec {
    int64_t interval_sec;
    int64_t interval_nsec;
    int64_t value_sec;
    int64_t value_nsec;
} lpr_linux_itimerspec_t;

static int lpr_timerfd_timespec_valid(int64_t sec, int64_t nsec)
{
    return sec >= 0 && nsec >= 0 && nsec < 1000000000ll;
}

static int lpr_timerfd_to_ns(int64_t sec, int64_t nsec, uint64_t *out)
{
    if (out == 0 || !lpr_timerfd_timespec_valid(sec, nsec) ||
        (uint64_t)sec > (UINT64_MAX - (uint64_t)nsec) / 1000000000ull)
    {
        return 0;
    }
    *out = (uint64_t)sec * 1000000000ull + (uint64_t)nsec;
    return 1;
}

static void lpr_timerfd_from_ns(uint64_t ns, int64_t *sec, int64_t *nsec)
{
    *sec = (int64_t)(ns / 1000000000ull);
    *nsec = (int64_t)(ns % 1000000000ull);
}

static int64_t lpr_timerfd_now_ns(const lpr_event_backend_t *timer, uint64_t *out)
{
    struct pachaos_timespec now;
    const int64_t status = lpr_pacha_clock_gettime((uint64_t)(uint32_t)timer->clock_id, &now);
    if (status != 0) {
        return status;
    }
    if (now.tv_sec > (UINT64_MAX - now.tv_nsec) / 1000000000ull) {
        return -LPR_LINUX_EOVERFLOW;
    }
    *out = now.tv_sec * 1000000000ull + now.tv_nsec;
    return 0;
}

static int64_t lpr_timerfd_update(lpr_event_backend_t *timer, uint64_t *out_now)
{
    uint64_t now = 0;
    const int64_t status = lpr_timerfd_now_ns(timer, &now);
    if (status != 0) {
        return status;
    }
    if (out_now != 0) {
        *out_now = now;
    }
    if (timer->deadline_ns == 0 || now < timer->deadline_ns) {
        return 0;
    }
    uint64_t expirations = 1;
    if (timer->interval_ns != 0) {
        expirations += (now - timer->deadline_ns) / timer->interval_ns;
        const __uint128_t next = (__uint128_t)timer->deadline_ns +
            (__uint128_t)expirations * timer->interval_ns;
        timer->deadline_ns = next > UINT64_MAX ? UINT64_MAX : (uint64_t)next;
    } else {
        timer->deadline_ns = 0;
    }
    timer->counter = timer->counter > UINT64_MAX - expirations ?
        UINT64_MAX : timer->counter + expirations;
    return 0;
}

int lpr_linux_timerfd_active(uint64_t fd)
{
    const lpr_event_backend_t *event = lpr_event_backend(fd);
    return event != 0 && event->active && event->subtype == LPR_EVENT_BACKEND_TIMERFD;
}

int64_t lpr_linux_timerfd_create(uint64_t clock_id, uint64_t flags)
{
    if (clock_id != LPR_LINUX_CLOCK_REALTIME && clock_id != LPR_LINUX_CLOCK_MONOTONIC) {
        return -LPR_LINUX_EINVAL;
    }
    if ((flags & ~(LPR_TIMERFD_CLOEXEC | LPR_TIMERFD_NONBLOCK)) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    const int fd = lpr_fd_slot_alloc();
    if (fd < 0) {
        return fd;
    }
    const int status = lpr_control_install_fd(
        (uint64_t)(uint32_t)fd,
        LPR_FD_OPS_EVENT,
        flags,
        (uint64_t)(uint32_t)fd,
        0);
    if (status != 0) {
        return status;
    }
    lpr_event_backend_t *timer = lpr_event_backend((uint64_t)(uint32_t)fd);
    if (timer == 0) {
        lpr_control_close_fd((uint64_t)(uint32_t)fd);
        return -LPR_LINUX_EIO;
    }
    timer->active = 1;
    timer->subtype = LPR_EVENT_BACKEND_TIMERFD;
    timer->flags = (uint32_t)flags;
    timer->clock_id = (int32_t)clock_id;
    return fd;
}

int64_t lpr_linux_timerfd_gettime(uint64_t fd, uint64_t current_value_raw)
{
    if (!lpr_linux_timerfd_active(fd)) {
        return -LPR_LINUX_EBADF;
    }
    if (current_value_raw == 0) {
        return -LPR_LINUX_EFAULT;
    }
    lpr_event_backend_t *timer = lpr_event_backend(fd);
    uint64_t now = 0;
    const int64_t status = lpr_timerfd_update(timer, &now);
    if (status != 0) {
        return status;
    }
    lpr_linux_itimerspec_t *current =
        (lpr_linux_itimerspec_t *)(uintptr_t)current_value_raw;
    lpr_memset(current, 0, sizeof(*current));
    lpr_timerfd_from_ns(timer->interval_ns, &current->interval_sec, &current->interval_nsec);
    const uint64_t remaining = timer->deadline_ns > now ? timer->deadline_ns - now : 0;
    lpr_timerfd_from_ns(remaining, &current->value_sec, &current->value_nsec);
    return 0;
}

int64_t lpr_linux_timerfd_settime(
    uint64_t fd,
    uint64_t flags,
    uint64_t new_value_raw,
    uint64_t old_value_raw)
{
    if (!lpr_linux_timerfd_active(fd)) {
        return -LPR_LINUX_EBADF;
    }
    if ((flags & ~LPR_LINUX_TIMER_ABSTIME) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    if (new_value_raw == 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (old_value_raw != 0) {
        const int64_t old_status = lpr_linux_timerfd_gettime(fd, old_value_raw);
        if (old_status != 0) {
            return old_status;
        }
    }
    const lpr_linux_itimerspec_t *value =
        (const lpr_linux_itimerspec_t *)(uintptr_t)new_value_raw;
    uint64_t interval_ns = 0;
    uint64_t value_ns = 0;
    if (!lpr_timerfd_to_ns(value->interval_sec, value->interval_nsec, &interval_ns) ||
        !lpr_timerfd_to_ns(value->value_sec, value->value_nsec, &value_ns))
    {
        return -LPR_LINUX_EINVAL;
    }
    lpr_event_backend_t *timer = lpr_event_backend(fd);
    timer->counter = 0;
    timer->interval_ns = interval_ns;
    timer->deadline_ns = 0;
    if (value_ns == 0) {
        return 0;
    }
    if ((flags & LPR_LINUX_TIMER_ABSTIME) != 0) {
        timer->deadline_ns = value_ns;
        return 0;
    }
    uint64_t now = 0;
    const int64_t status = lpr_timerfd_now_ns(timer, &now);
    if (status != 0) {
        return status;
    }
    timer->deadline_ns = value_ns > UINT64_MAX - now ? UINT64_MAX : now + value_ns;
    return 0;
}

int64_t lpr_linux_timerfd_read(uint64_t fd, uint64_t buf, uint64_t count)
{
    if (!lpr_linux_timerfd_active(fd)) {
        return -LPR_LINUX_EBADF;
    }
    if (count < sizeof(uint64_t)) {
        return -LPR_LINUX_EINVAL;
    }
    if (buf == 0) {
        return -LPR_LINUX_EFAULT;
    }
    lpr_event_backend_t *timer = lpr_event_backend(fd);
    for (;;) {
        const int64_t status = lpr_timerfd_update(timer, 0);
        if (status != 0) {
            return status;
        }
        if (timer->counter != 0) {
            *(uint64_t *)(uintptr_t)buf = timer->counter;
            timer->counter = 0;
            return (int64_t)sizeof(uint64_t);
        }
        if ((timer->flags & LPR_TIMERFD_NONBLOCK) != 0) {
            return -LPR_LINUX_EAGAIN;
        }
        const struct pachaos_timespec delay = { .tv_sec = 0, .tv_nsec = 1000000 };
        const int64_t sleep_status = lpr_pacha_nanosleep(&delay);
        if (sleep_status != 0) {
            return sleep_status;
        }
    }
}

uint32_t lpr_linux_timerfd_poll_events(uint64_t fd, uint32_t events)
{
    if (!lpr_linux_timerfd_active(fd)) {
        return 0;
    }
    lpr_event_backend_t *timer = lpr_event_backend(fd);
    if (lpr_timerfd_update(timer, 0) != 0) {
        return 0x0008u;
    }
    return (events & 0x0001u) != 0 && timer->counter != 0 ? 0x0001u : 0u;
}
