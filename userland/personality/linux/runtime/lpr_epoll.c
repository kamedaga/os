#include "lpr_epoll.h"

#include "lpr_filed_internal.h"

#define LPR_EPOLL_INSTANCE_BYTES 4096ull
#define LPR_EPOLL_INSTANCE_MAGIC 0x314c4c4f5045504cull
#define LPR_EPOLL_WAIT_QUANTUM_MS 10ull
#define LPR_EPOLL_NATIVE_WAIT_MAX 64u

#define LPR_EPOLL_CLOEXEC 02000000u
#define LPR_EPOLLIN 0x0001u
#define LPR_EPOLLOUT 0x0004u
#define LPR_EPOLLERR 0x0008u
#define LPR_EPOLLHUP 0x0010u
#define LPR_EPOLLEXCLUSIVE (1u << 28)
#define LPR_EPOLLWAKEUP (1u << 29)
#define LPR_EPOLLONESHOT (1u << 30)
#define LPR_EPOLLET (1u << 31)

#define LPR_EPOLL_CTL_ADD 1u
#define LPR_EPOLL_CTL_DEL 2u
#define LPR_EPOLL_CTL_MOD 3u

#define LPR_USER_LOW_GUARD_END 4096ull
#define LPR_USER_CANONICAL_END 0x0000800000000000ull

typedef struct lpr_linux_epoll_event {
    uint32_t events;
    uint64_t data;
} __attribute__((packed)) lpr_linux_epoll_event_t;

typedef struct lpr_linux_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
} lpr_linux_pollfd_t;

typedef struct lpr_epoll_interest {
    uint32_t target_fd;
    uint32_t target_ofd_index;
    uint64_t target_generation;
    uint32_t events;
    uint32_t reserved0;
    uint64_t data;
} lpr_epoll_interest_t;

typedef struct lpr_epoll_instance {
    uint64_t magic;
    uint64_t generation;
    uint32_t count;
    uint32_t capacity;
    uint64_t reserved0;
    lpr_epoll_interest_t interests[];
} lpr_epoll_instance_t;

enum {
    LPR_EPOLL_MAX_INTERESTS =
        (LPR_EPOLL_INSTANCE_BYTES - sizeof(lpr_epoll_instance_t)) /
        sizeof(lpr_epoll_interest_t),
};

typedef struct lpr_epoll_snapshot {
    int32_t fd;
    uint8_t kind;
    uint8_t reserved0;
    uint16_t reserved1;
    uint32_t events;
    uint64_t data;
} lpr_epoll_snapshot_t;

static int lpr_epoll_user_range_plausible(uint64_t ptr, uint64_t bytes)
{
    if (bytes == 0) {
        return 1;
    }
    if (ptr < LPR_USER_LOW_GUARD_END || ptr > UINT64_MAX - (bytes - 1u)) {
        return 0;
    }
    const uint64_t end = ptr + bytes - 1u;
    return ptr < LPR_USER_CANONICAL_END && end < LPR_USER_CANONICAL_END;
}

static lpr_epoll_instance_t *lpr_epoll_instance_for_object(lpr_ofd_t *object)
{
    if (object == 0 ||
        lpr_ofd_ops_id(object) != LPR_FD_OPS_EPOLL ||
        ((lpr_epoll_backend_t *)lpr_backend_state_from_ofd(object))->instance < LPR_USER_LOW_GUARD_END ||
        ((lpr_epoll_backend_t *)lpr_backend_state_from_ofd(object))->map_bytes != LPR_EPOLL_INSTANCE_BYTES)
    {
        return 0;
    }
    lpr_epoll_instance_t *instance =
        (lpr_epoll_instance_t *)(uintptr_t)((lpr_epoll_backend_t *)lpr_backend_state_from_ofd(object))->instance;
    if (instance->magic != LPR_EPOLL_INSTANCE_MAGIC ||
        instance->capacity != LPR_EPOLL_MAX_INTERESTS ||
        instance->count > instance->capacity)
    {
        return 0;
    }
    return instance;
}

static void lpr_epoll_remove_interest(lpr_epoll_instance_t *instance, uint32_t index)
{
    if (instance == 0 || index >= instance->count) {
        return;
    }
    for (uint32_t i = index + 1u; i < instance->count; i++) {
        instance->interests[i - 1u] = instance->interests[i];
    }
    instance->count--;
    lpr_memset(&instance->interests[instance->count], 0, sizeof(instance->interests[0]));
    instance->generation++;
}

int lpr_linux_epoll_fd_active(uint64_t fd)
{
    return lpr_epoll_backend(fd) != 0;
}

int64_t lpr_linux_epoll_create1(uint64_t flags)
{
    if ((flags & ~((uint64_t)LPR_EPOLL_CLOEXEC)) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    const int fd = lpr_fd_slot_alloc();
    if (fd < 0) {
        return fd;
    }
    const int64_t mapped = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_MMAP,
        0,
        0,
        LPR_EPOLL_INSTANCE_BYTES,
        PACHAOS_PROT_READ | PACHAOS_PROT_WRITE,
        PACHAOS_MMAP_PRIVATE | PACHAOS_MMAP_ANONYMOUS,
        0);
    if (mapped < (int64_t)LPR_USER_LOW_GUARD_END) {
        return lpr_pacha_status_to_errno(mapped);
    }
    lpr_epoll_instance_t *instance = (lpr_epoll_instance_t *)(uintptr_t)mapped;
    lpr_memset(instance, 0, LPR_EPOLL_INSTANCE_BYTES);
    instance->magic = LPR_EPOLL_INSTANCE_MAGIC;
    instance->generation = 1;
    instance->capacity = LPR_EPOLL_MAX_INTERESTS;

    const int status = lpr_control_install_fd(
        (uint64_t)(uint32_t)fd,
        LPR_FD_OPS_EPOLL,
        flags,
        (uint64_t)(uintptr_t)instance,
        LPR_EPOLL_INSTANCE_BYTES);
    if (status != 0) {
        (void)lpr_pacha_syscall2(
            PACHAOS_SYSCALL_MUNMAP,
            (uint64_t)(uintptr_t)instance,
            LPR_EPOLL_INSTANCE_BYTES);
        return status;
    }
    return fd;
}

static int lpr_epoll_interest_matches(
    const lpr_epoll_interest_t *interest,
    uint32_t target_fd,
    uint32_t target_ofd_index,
    uint64_t target_generation)
{
    return interest->target_fd == target_fd &&
        interest->target_ofd_index == target_ofd_index &&
        interest->target_generation == target_generation;
}

static int lpr_epoll_reaches_unlocked(
    uint32_t ofd_index,
    uint32_t wanted_ofd_index,
    uint32_t depth)
{
    if (ofd_index == wanted_ofd_index) {
        return 1;
    }
    if (ofd_index >= lpr_control_fd_table.ofd_count ||
        depth >= lpr_control_fd_table.ofd_count)
    {
        return 0;
    }
    lpr_ofd_t *object = &lpr_control_fd_table.ofds[ofd_index];
    lpr_epoll_instance_t *instance = lpr_epoll_instance_for_object(object);
    if (instance == 0) {
        return 0;
    }
    for (uint32_t i = 0; i < instance->count; i++) {
        const lpr_epoll_interest_t *interest = &instance->interests[i];
        if (interest->target_ofd_index >= lpr_control_fd_table.ofd_count) {
            continue;
        }
        const lpr_ofd_t *target =
            &lpr_control_fd_table.ofds[interest->target_ofd_index];
        if (!target->active || target->generation != interest->target_generation ||
            lpr_ofd_ops_id(target) != LPR_FD_OPS_EPOLL)
        {
            continue;
        }
        if (lpr_epoll_reaches_unlocked(
                interest->target_ofd_index,
                wanted_ofd_index,
                depth + 1u))
        {
            return 1;
        }
    }
    return 0;
}

int64_t lpr_linux_epoll_ctl(uint64_t epfd_raw, uint64_t op, uint64_t fd_raw, uint64_t event_raw)
{
    if (epfd_raw > LPR_LINUX_FD_MAX || fd_raw > LPR_LINUX_FD_MAX) {
        return -LPR_LINUX_EBADF;
    }
    if (op != LPR_EPOLL_CTL_ADD && op != LPR_EPOLL_CTL_MOD && op != LPR_EPOLL_CTL_DEL) {
        return -LPR_LINUX_EINVAL;
    }
    if (epfd_raw == fd_raw) {
        return -LPR_LINUX_EINVAL;
    }

    lpr_linux_epoll_event_t event;
    lpr_memset(&event, 0, sizeof(event));
    if (op != LPR_EPOLL_CTL_DEL) {
        if (!lpr_epoll_user_range_plausible(event_raw, sizeof(event))) {
            return -LPR_LINUX_EFAULT;
        }
        lpr_memcpy(&event, (const void *)(uintptr_t)event_raw, sizeof(event));
        if ((event.events & (LPR_EPOLLET | LPR_EPOLLONESHOT | LPR_EPOLLEXCLUSIVE)) != 0) {
            return -LPR_LINUX_EINVAL;
        }
        event.events &= ~LPR_EPOLLWAKEUP;
    }

    const uint32_t epfd = (uint32_t)epfd_raw;
    const uint32_t fd = (uint32_t)fd_raw;
    lpr_fd_arrays_init();
    lpr_fd_table_lock(&lpr_control_fd_table);
    if (epfd >= lpr_control_fd_table.entry_count ||
        !lpr_control_fd_table.entries[epfd].active)
    {
        lpr_fd_table_unlock(&lpr_control_fd_table);
        return -LPR_LINUX_EBADF;
    }
    lpr_fd_entry_t *epoll_slot = &lpr_control_fd_table.entries[epfd];
    if (epoll_slot->ofd_index >= lpr_control_fd_table.ofd_count) {
        lpr_fd_table_unlock(&lpr_control_fd_table);
        return -LPR_LINUX_EBADF;
    }
    lpr_ofd_t *epoll_object = &lpr_control_fd_table.ofds[epoll_slot->ofd_index];
    lpr_epoll_instance_t *instance = lpr_epoll_instance_for_object(epoll_object);
    if (instance == 0) {
        lpr_fd_table_unlock(&lpr_control_fd_table);
        return epoll_object->active ? -LPR_LINUX_EINVAL : -LPR_LINUX_EBADF;
    }
    if (fd >= lpr_control_fd_table.entry_count || !lpr_control_fd_table.entries[fd].active) {
        lpr_fd_table_unlock(&lpr_control_fd_table);
        return -LPR_LINUX_EBADF;
    }
    const lpr_fd_entry_t *target_slot = &lpr_control_fd_table.entries[fd];
    if (target_slot->ofd_index >= lpr_control_fd_table.ofd_count) {
        lpr_fd_table_unlock(&lpr_control_fd_table);
        return -LPR_LINUX_EBADF;
    }
    const lpr_ofd_t *target = &lpr_control_fd_table.ofds[target_slot->ofd_index];
    if (!target->active) {
        lpr_fd_table_unlock(&lpr_control_fd_table);
        return -LPR_LINUX_EBADF;
    }
    if (lpr_ofd_ops_id(target) != LPR_FD_OPS_FILED &&
        lpr_ofd_ops_id(target) != LPR_FD_OPS_PIPE &&
        lpr_ofd_ops_id(target) != LPR_FD_OPS_SOCKET &&
        lpr_ofd_ops_id(target) != LPR_FD_OPS_EVENT &&
        lpr_ofd_ops_id(target) != LPR_FD_OPS_TTY &&
        lpr_ofd_ops_id(target) != LPR_FD_OPS_DRM &&
        lpr_ofd_ops_id(target) != LPR_FD_OPS_INPUT &&
        lpr_ofd_ops_id(target) != LPR_FD_OPS_EPOLL)
    {
        lpr_fd_table_unlock(&lpr_control_fd_table);
        return -LPR_LINUX_EPERM;
    }

    uint32_t found = instance->count;
    for (uint32_t i = 0; i < instance->count; i++) {
        if (lpr_epoll_interest_matches(
                &instance->interests[i],
                fd,
                target_slot->ofd_index,
                target->generation))
        {
            found = i;
            break;
        }
    }
    if (op == LPR_EPOLL_CTL_ADD) {
        if (found != instance->count) {
            lpr_fd_table_unlock(&lpr_control_fd_table);
            return -LPR_LINUX_EEXIST;
        }
        if (lpr_ofd_ops_id(target) == LPR_FD_OPS_EPOLL &&
            lpr_epoll_reaches_unlocked(
                target_slot->ofd_index,
                epoll_slot->ofd_index,
                0))
        {
            lpr_fd_table_unlock(&lpr_control_fd_table);
            return -LPR_LINUX_ELOOP;
        }
        if (instance->count >= instance->capacity) {
            lpr_fd_table_unlock(&lpr_control_fd_table);
            return -LPR_LINUX_ENOSPC;
        }
        lpr_epoll_interest_t *interest = &instance->interests[instance->count++];
        lpr_memset(interest, 0, sizeof(*interest));
        interest->target_fd = fd;
        interest->target_ofd_index = target_slot->ofd_index;
        interest->target_generation = target->generation;
        interest->events = event.events;
        interest->data = event.data;
        instance->generation++;
        lpr_fd_table_unlock(&lpr_control_fd_table);
        return 0;
    }
    if (found == instance->count) {
        lpr_fd_table_unlock(&lpr_control_fd_table);
        return -LPR_LINUX_ENOENT;
    }
    if (op == LPR_EPOLL_CTL_DEL) {
        lpr_epoll_remove_interest(instance, found);
    } else {
        instance->interests[found].events = event.events;
        instance->interests[found].data = event.data;
        instance->generation++;
    }
    lpr_fd_table_unlock(&lpr_control_fd_table);
    return 0;
}

static int lpr_epoll_resolve_target_fd_unlocked(const lpr_epoll_interest_t *interest)
{
    if (interest->target_ofd_index >= lpr_control_fd_table.ofd_count) {
        return -1;
    }
    const lpr_ofd_t *target =
        &lpr_control_fd_table.ofds[interest->target_ofd_index];
    if (!target->active || target->generation != interest->target_generation) {
        return -1;
    }
    if (interest->target_fd < lpr_control_fd_table.entry_count) {
        const lpr_fd_entry_t *slot =
            &lpr_control_fd_table.entries[interest->target_fd];
        if (slot->active && slot->ofd_index == interest->target_ofd_index) {
            return (int)interest->target_fd;
        }
    }
    for (uint32_t fd = 0; fd < lpr_control_fd_table.entry_count; fd++) {
        const lpr_fd_entry_t *slot = &lpr_control_fd_table.entries[fd];
        if (slot->active && slot->ofd_index == interest->target_ofd_index) {
            return (int)fd;
        }
    }
    return -1;
}

static int64_t lpr_epoll_snapshot(
    uint32_t epfd,
    lpr_epoll_snapshot_t *snapshot,
    uint32_t *out_count)
{
    *out_count = 0;
    lpr_fd_table_lock(&lpr_control_fd_table);
    if (epfd >= lpr_control_fd_table.entry_count ||
        !lpr_control_fd_table.entries[epfd].active)
    {
        lpr_fd_table_unlock(&lpr_control_fd_table);
        return -LPR_LINUX_EBADF;
    }
    const lpr_fd_entry_t *slot = &lpr_control_fd_table.entries[epfd];
    if (slot->ofd_index >= lpr_control_fd_table.ofd_count) {
        lpr_fd_table_unlock(&lpr_control_fd_table);
        return -LPR_LINUX_EBADF;
    }
    lpr_ofd_t *object = &lpr_control_fd_table.ofds[slot->ofd_index];
    lpr_epoll_instance_t *instance = lpr_epoll_instance_for_object(object);
    if (instance == 0) {
        lpr_fd_table_unlock(&lpr_control_fd_table);
        return object->active ? -LPR_LINUX_EINVAL : -LPR_LINUX_EBADF;
    }
    for (uint32_t i = 0; i < instance->count; i++) {
        const lpr_epoll_interest_t *interest = &instance->interests[i];
        const int target_fd = lpr_epoll_resolve_target_fd_unlocked(interest);
        if (target_fd < 0) {
            continue;
        }
        const lpr_ofd_t *target =
            &lpr_control_fd_table.ofds[interest->target_ofd_index];
        lpr_epoll_snapshot_t *item = &snapshot[*out_count];
        item->fd = target_fd;
        item->kind = lpr_ofd_ops_id(target);
        item->events = interest->events;
        item->data = interest->data;
        (*out_count)++;
    }
    lpr_fd_table_unlock(&lpr_control_fd_table);
    return 0;
}

static int64_t lpr_epoll_scan(
    const lpr_epoll_snapshot_t *snapshot,
    uint32_t count,
    lpr_linux_epoll_event_t *events,
    uint32_t maxevents)
{
    lpr_linux_pollfd_t pollfds[LPR_EPOLL_MAX_INTERESTS];
    for (uint32_t i = 0; i < count; i++) {
        pollfds[i].fd = snapshot[i].kind == LPR_FD_OPS_EPOLL ?
            -1 : snapshot[i].fd;
        pollfds[i].events = (int16_t)(snapshot[i].events | LPR_EPOLLERR | LPR_EPOLLHUP);
        pollfds[i].revents = 0;
    }
    const int64_t poll_status = lpr_linux_poll(
        (uint64_t)(uintptr_t)pollfds,
        count,
        0);
    if (poll_status < 0) {
        return poll_status;
    }
    uint32_t ready = 0;
    for (uint32_t i = 0; i < count && ready < maxevents; i++) {
        if (snapshot[i].kind == LPR_FD_OPS_EPOLL) {
            lpr_linux_epoll_event_t nested_event;
            const int64_t nested_ready = lpr_linux_epoll_wait(
                (uint64_t)(uint32_t)snapshot[i].fd,
                (uint64_t)(uintptr_t)&nested_event,
                1,
                0);
            if (nested_ready < 0) {
                return nested_ready;
            }
            if (nested_ready > 0 && (snapshot[i].events & LPR_EPOLLIN) != 0) {
                events[ready].events = LPR_EPOLLIN;
                events[ready].data = snapshot[i].data;
                ready++;
            }
            continue;
        }
        const uint32_t revents = (uint16_t)pollfds[i].revents;
        const uint32_t report = revents &
            (snapshot[i].events | LPR_EPOLLERR | LPR_EPOLLHUP) &
            (LPR_EPOLLIN | LPR_EPOLLOUT | LPR_EPOLLERR | LPR_EPOLLHUP);
        if (report == 0) {
            continue;
        }
        events[ready].events = report;
        events[ready].data = snapshot[i].data;
        ready++;
    }
    return ready;
}

static int64_t lpr_epoll_sleep_ms(uint64_t ms)
{
    struct pachaos_timespec ts = {
        .tv_sec = ms / 1000u,
        .tv_nsec = (ms % 1000u) * 1000000u,
    };
    const int64_t status = lpr_pacha_syscall1(
        PACHAOS_SYSCALL_NANOSLEEP,
        (uint64_t)(uintptr_t)&ts);
    return status == 0 ? 0 : lpr_pacha_status_to_errno(status);
}

static int64_t lpr_epoll_block(
    const lpr_epoll_snapshot_t *snapshot,
    uint32_t count,
    int64_t remaining_ms,
    uint64_t *out_waited_ms)
{
    struct pacha_pollfd pollfds[LPR_EPOLL_NATIVE_WAIT_MAX];
    uint32_t native_count = 0;
    for (uint32_t i = 0; i < count && native_count < LPR_EPOLL_NATIVE_WAIT_MAX; i++) {
        int native_fd = -1;
        if (snapshot[i].kind == LPR_FD_OPS_PIPE) native_fd = snapshot[i].fd;
        else if (snapshot[i].kind == LPR_FD_OPS_DRM)
            native_fd = lpr_drm_native_wait_fd((uint64_t)(uint32_t)snapshot[i].fd);
        else if (snapshot[i].kind == LPR_FD_OPS_INPUT)
            native_fd = lpr_input_native_wait_fd((uint64_t)(uint32_t)snapshot[i].fd);
        else if (snapshot[i].kind == LPR_FD_OPS_SOCKET)
            native_fd = lpr_linux_socket_native_wait_fd((uint64_t)(uint32_t)snapshot[i].fd);
        if (native_fd < 16) continue;
        lpr_memset(&pollfds[native_count], 0, sizeof(pollfds[native_count]));
        pollfds[native_count].fd = native_fd;
        pollfds[native_count].events = snapshot[i].kind == LPR_FD_OPS_PIPE ?
            lpr_pipe_poll_events_to_pacha(snapshot[i].events | LPR_EPOLLERR | LPR_EPOLLHUP) :
            PACHA_FD_EVENT_READABLE;
        native_count++;
    }
    uint64_t wait_ms = LPR_EPOLL_WAIT_QUANTUM_MS;
    const int all_native = count != 0 && native_count == count;
    if (all_native) wait_ms = remaining_ms < 0 ? UINT64_MAX : (uint64_t)remaining_ms;
    else if (remaining_ms > 0 && remaining_ms < (int64_t)wait_ms) wait_ms = (uint64_t)remaining_ms;
    if (out_waited_ms != 0) *out_waited_ms = wait_ms;
    if (native_count == 0) {
        return lpr_epoll_sleep_ms(wait_ms);
    }
    const int64_t status = lpr_pacha_syscall4(
        PACHAOS_SYSCALL_FD_WAIT_MANY,
        (uint64_t)(uintptr_t)pollfds,
        native_count,
        wait_ms,
        0);
    if (status >= 0) {
        if (out_waited_ms != 0) *out_waited_ms = 0;
        return 0;
    }
    if (status == PACHA_SYSCALL_ERR_NOT_READY ||
        status == -PACHA_SYSCALL_ERR_NOT_READY)
    {
        return 0;
    }
    return lpr_pacha_status_to_errno(status);
}

int64_t lpr_linux_epoll_wait(
    uint64_t epfd_raw,
    uint64_t events_raw,
    uint64_t maxevents_raw,
    uint64_t timeout_raw)
{
    if (epfd_raw > LPR_LINUX_FD_MAX) {
        return -LPR_LINUX_EBADF;
    }
    if (maxevents_raw == 0 || maxevents_raw > 0x7fffffffu ||
        maxevents_raw > UINT64_MAX / sizeof(lpr_linux_epoll_event_t) ||
        !lpr_epoll_user_range_plausible(
            events_raw,
            maxevents_raw * sizeof(lpr_linux_epoll_event_t)))
    {
        return maxevents_raw == 0 || maxevents_raw > 0x7fffffffu ?
            -LPR_LINUX_EINVAL : -LPR_LINUX_EFAULT;
    }
    const int64_t timeout = (int64_t)timeout_raw;
    if (timeout < -1) {
        return -LPR_LINUX_EINVAL;
    }

    lpr_fd_arrays_init();
    int64_t remaining_ms = timeout;
    for (;;) {
        lpr_epoll_snapshot_t snapshot[LPR_EPOLL_MAX_INTERESTS];
        uint32_t count = 0;
        const int64_t snapshot_status =
            lpr_epoll_snapshot((uint32_t)epfd_raw, snapshot, &count);
        if (snapshot_status != 0) {
            return snapshot_status;
        }
        const int64_t ready = lpr_epoll_scan(
            snapshot,
            count,
            (lpr_linux_epoll_event_t *)(uintptr_t)events_raw,
            (uint32_t)maxevents_raw);
        if (ready != 0 || timeout == 0) {
            return ready;
        }

        int64_t block_ms = remaining_ms;
        uint64_t waited_ms = 0;
        const int64_t wait_status = lpr_epoll_block(snapshot, count, block_ms, &waited_ms);
        lpr_linux_deliver_native_pending_frame(-LPR_LINUX_EINTR);
        if (wait_status != 0 && wait_status != -LPR_LINUX_EBADF) {
            return wait_status;
        }
        if (remaining_ms > 0 && waited_ms != UINT64_MAX) {
            remaining_ms -= (int64_t)waited_ms;
            if (remaining_ms <= 0) {
                return 0;
            }
        }
    }
}

int64_t lpr_linux_epoll_pwait(
    uint64_t epfd,
    uint64_t events,
    uint64_t maxevents,
    uint64_t timeout,
    uint64_t sigmask,
    uint64_t sigsetsize)
{
    uint64_t old_mask = 0;
    if (sigmask != 0) {
        if (sigsetsize != sizeof(uint64_t) ||
            !lpr_epoll_user_range_plausible(sigmask, sizeof(uint64_t)))
        {
            return sigsetsize != sizeof(uint64_t) ?
                -LPR_LINUX_EINVAL : -LPR_LINUX_EFAULT;
        }
        const int64_t mask_status = lpr_linux_rt_sigprocmask(
            LPR_LINUX_SIG_SETMASK,
            sigmask,
            (uint64_t)(uintptr_t)&old_mask,
            sizeof(old_mask));
        if (mask_status != 0) {
            return mask_status;
        }
    }
    const int64_t result = lpr_linux_epoll_wait(epfd, events, maxevents, timeout);
    if (sigmask != 0) {
        (void)lpr_linux_rt_sigprocmask(
            LPR_LINUX_SIG_SETMASK,
            (uint64_t)(uintptr_t)&old_mask,
            0,
            sizeof(old_mask));
    }
    return result;
}

void lpr_epoll_before_close(uint64_t fd_raw)
{
    if (fd_raw > LPR_LINUX_FD_MAX) {
        return;
    }
    const uint32_t fd = (uint32_t)fd_raw;
    lpr_fd_table_lock(&lpr_control_fd_table);
    if (fd >= lpr_control_fd_table.entry_count || !lpr_control_fd_table.entries[fd].active) {
        lpr_fd_table_unlock(&lpr_control_fd_table);
        return;
    }
    const lpr_fd_entry_t *slot = &lpr_control_fd_table.entries[fd];
    if (slot->ofd_index >= lpr_control_fd_table.ofd_count) {
        lpr_fd_table_unlock(&lpr_control_fd_table);
        return;
    }
    lpr_ofd_t *closing = &lpr_control_fd_table.ofds[slot->ofd_index];
    if (!closing->active || closing->refcount == 0) {
        lpr_fd_table_unlock(&lpr_control_fd_table);
        return;
    }
    if (closing->refcount == 1) {
        for (uint32_t i = 0; i < lpr_control_fd_table.ofd_count; i++) {
            lpr_ofd_t *object = &lpr_control_fd_table.ofds[i];
            lpr_epoll_instance_t *instance = lpr_epoll_instance_for_object(object);
            if (instance == 0) {
                continue;
            }
            for (uint32_t j = 0; j < instance->count;) {
                const lpr_epoll_interest_t *interest = &instance->interests[j];
                if (interest->target_ofd_index == slot->ofd_index &&
                    interest->target_generation == closing->generation)
                {
                    lpr_epoll_remove_interest(instance, j);
                    continue;
                }
                j++;
            }
        }
    }
    lpr_fd_table_unlock(&lpr_control_fd_table);
}
