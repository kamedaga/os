#include "lpr_wait.h"

#include "lpr_filed_internal.h"

#define LPR_WAIT_NS_PER_MS 1000000ull
#define LPR_WAIT_NS_PER_SEC 1000000000ull

enum {
    LPR_WAIT_DRAIN_NONE = 0u,
    LPR_WAIT_DRAIN_NATIVE = 1u,
    LPR_WAIT_DRAIN_EVENT = 2u,
};

static int64_t lpr_wait_now_ns(uint64_t *out_now)
{
    if (out_now == 0) return -LPR_LINUX_EFAULT;
    struct pachaos_timespec now;
    const int64_t status = lpr_pacha_clock_gettime(
        LPR_LINUX_CLOCK_MONOTONIC, &now);
    if (status != 0) return status;
    if (now.tv_sec > (UINT64_MAX - now.tv_nsec) / LPR_WAIT_NS_PER_SEC)
        return -LPR_LINUX_EOVERFLOW;
    *out_now = now.tv_sec * LPR_WAIT_NS_PER_SEC + now.tv_nsec;
    return 0;
}

void lpr_wait_graph_init(lpr_wait_graph_t *graph)
{
    if (graph == 0) return;
    lpr_memset(graph, 0, sizeof(*graph));
    graph->relative_deadline_ns = UINT64_MAX;
}

static int64_t lpr_wait_graph_add_leaf(
    lpr_wait_graph_t *graph,
    int native_fd,
    uint64_t events,
    uint64_t min_write_bytes,
    uint8_t drain_mode,
    uint32_t logical_fd)
{
    if (graph == 0 || native_fd < 16) return -LPR_LINUX_EBADF;
    for (uint32_t i = 0; i < graph->leaf_count; ++i) {
        if (graph->leaves[i].fd != native_fd) continue;
        graph->leaves[i].events |= events;
        if ((events & PACHA_FD_EVENT_WRITABLE) != 0 &&
            min_write_bytes > graph->leaves[i].revents)
            graph->leaves[i].revents = min_write_bytes;
        if (drain_mode > graph->drain_modes[i]) {
            graph->drain_modes[i] = drain_mode;
            graph->logical_fds[i] = logical_fd;
        }
        return 0;
    }
    if (graph->leaf_count >= LPR_WAIT_GRAPH_MAX_LEAVES)
        return -LPR_LINUX_ENOSPC;
    const uint32_t index = graph->leaf_count++;
    graph->leaves[index] = (struct pacha_pollfd){
        .fd = native_fd,
        .events = events,
        .revents = (events & PACHA_FD_EVENT_WRITABLE) != 0 ?
            min_write_bytes : 0,
    };
    graph->drain_modes[index] = drain_mode;
    graph->logical_fds[index] = logical_fd;
    return 0;
}

int64_t lpr_wait_graph_add_native(
    lpr_wait_graph_t *graph,
    int native_fd,
    uint64_t events)
{
    return lpr_wait_graph_add_leaf(
        graph, native_fd, events, 0, LPR_WAIT_DRAIN_NONE, UINT32_MAX);
}

int64_t lpr_wait_graph_add_native_min(
    lpr_wait_graph_t *graph,
    int native_fd,
    uint64_t events,
    uint64_t min_write_bytes)
{
    return lpr_wait_graph_add_leaf(
        graph,
        native_fd,
        events,
        min_write_bytes,
        LPR_WAIT_DRAIN_NONE,
        UINT32_MAX);
}

int64_t lpr_wait_graph_add_control(
    lpr_wait_graph_t *graph,
    int native_fd)
{
    return lpr_wait_graph_add_leaf(
        graph, native_fd, PACHA_FD_EVENT_READABLE,
        0, LPR_WAIT_DRAIN_NATIVE, UINT32_MAX);
}

static void lpr_wait_graph_add_relative_deadline(
    lpr_wait_graph_t *graph,
    uint64_t remaining_ns)
{
    if (remaining_ns < graph->relative_deadline_ns)
        graph->relative_deadline_ns = remaining_ns;
}

int64_t lpr_wait_graph_add_fd(
    lpr_wait_graph_t *graph,
    uint64_t fd,
    uint32_t events)
{
    if (graph == 0) return -LPR_LINUX_EFAULT;
    if (lpr_linux_pipe_fd_active(fd)) {
        const lpr_pipe_backend_t *pipe = lpr_pipe_backend(fd);
        return pipe != 0 ? lpr_wait_graph_add_native(
            graph,
            pipe->native.raw,
            lpr_pipe_poll_events_to_pacha(events | 0x0008u)) :
            -LPR_LINUX_EBADF;
    }
    if (lpr_linux_tty_fd_active(fd)) {
        const lpr_tty_backend_t *tty = lpr_tty_backend(fd);
        return tty != 0 ? lpr_wait_graph_add_leaf(
            graph, tty->wait_fd.raw, PACHA_FD_EVENT_READABLE,
            0, LPR_WAIT_DRAIN_NATIVE, (uint32_t)fd) :
            -LPR_LINUX_EBADF;
    }
    if (lpr_linux_drm_fd_active(fd))
        return lpr_wait_graph_add_native(
            graph, lpr_drm_native_wait_fd(fd), PACHA_FD_EVENT_READABLE);
    if (lpr_linux_input_fd_active(fd))
        return lpr_wait_graph_add_leaf(
            graph, lpr_input_native_wait_fd(fd), PACHA_FD_EVENT_READABLE,
            /*
             * The channel token represents durable unread input state.
             * Keep it readable across the post-wake poll rescan; the input
             * read path drains it immediately before asking inputd for data.
             */
            0, LPR_WAIT_DRAIN_NONE, (uint32_t)fd);
    if (lpr_linux_sync_file_fd_active(fd))
        return (events & 0x0001u) != 0 ? lpr_wait_graph_add_native(
            graph, lpr_sync_file_native_wait_fd(fd),
            PACHA_FD_EVENT_READABLE) : 0;
    if (lpr_linux_epoll_fd_active(fd))
        return lpr_epoll_add_wait_graph(fd, graph);
    if (lpr_linux_socket_fd_active(fd)) {
        const int native_fd = lpr_linux_socket_native_wait_fd(fd);
#if defined(LPR_GLYCIN_DIAG) && LPR_GLYCIN_DIAG
        if (0 && __atomic_load_n(&lpr_glycin_diag_armed, __ATOMIC_ACQUIRE) != 0u &&
            __atomic_load_n(
                &lpr_glycin_diag_socket_fd, __ATOMIC_ACQUIRE) == (uint32_t)fd)
        {
            lpr_glycin_diag_event(
                "wait.add.socket", fd, (uint64_t)(uint32_t)native_fd,
                events, graph->leaf_count);
        }
#endif
        return lpr_wait_graph_add_leaf(
            graph, native_fd, PACHA_FD_EVENT_READABLE,
            0, LPR_WAIT_DRAIN_NATIVE, (uint32_t)fd);
    }
    if (lpr_linux_timerfd_active(fd)) {
        if ((events & 0x0001u) == 0)
            return 0;
        uint64_t remaining_ns = UINT64_MAX;
        const int64_t status = lpr_timerfd_remaining_ns(fd, &remaining_ns);
        if (status != 0) return status;
        if (remaining_ns != UINT64_MAX)
            lpr_wait_graph_add_relative_deadline(graph, remaining_ns);
        return lpr_wait_graph_add_leaf(
            graph, lpr_eventfd_native_wait_fd(fd), PACHA_FD_EVENT_READABLE,
            0, LPR_WAIT_DRAIN_EVENT, (uint32_t)fd);
    }
    if (lpr_linux_signalfd_active(fd)) {
        if ((events & 0x0001u) == 0)
            return 0;
        return lpr_wait_graph_add_leaf(
            graph, lpr_eventfd_native_wait_fd(fd), PACHA_FD_EVENT_READABLE,
            0, LPR_WAIT_DRAIN_EVENT, (uint32_t)fd);
    }
    if (lpr_linux_eventfd_active(fd)) {
        const int native_fd = lpr_eventfd_native_wait_fd(fd);
#if defined(LPR_GLYCIN_DIAG) && LPR_GLYCIN_DIAG
        if (__atomic_load_n(&lpr_glycin_diag_armed, __ATOMIC_ACQUIRE) != 0u) {
            const lpr_event_backend_t *event = lpr_event_backend(fd);
            lpr_glycin_diag_event(
                "event.wait.add", fd,
                event != 0 ? event->counter : 0,
                event != 0 ? event->notify_pending : 0,
                native_fd);
        }
#endif
        return lpr_wait_graph_add_leaf(
            graph, native_fd, PACHA_FD_EVENT_READABLE,
            0, LPR_WAIT_DRAIN_EVENT, (uint32_t)fd);
    }
    if (lpr_linux_filed_fd_active(fd) || lpr_linux_device_fd_active(fd))
        return 0;
    return -LPR_LINUX_EBADF;
}

int64_t lpr_wait_deadline_init(
    lpr_wait_deadline_t *deadline,
    int64_t timeout_ms)
{
    if (deadline == 0) return -LPR_LINUX_EFAULT;
    lpr_memset(deadline, 0, sizeof(*deadline));
    if (timeout_ms < 0) return 0;
    uint64_t now = 0;
    const int64_t status = lpr_wait_now_ns(&now);
    if (status != 0) return status;
    const uint64_t timeout_ns = (uint64_t)timeout_ms > UINT64_MAX / LPR_WAIT_NS_PER_MS ?
        UINT64_MAX : (uint64_t)timeout_ms * LPR_WAIT_NS_PER_MS;
    deadline->finite = 1;
    deadline->expires_ns = timeout_ns > UINT64_MAX - now ?
        UINT64_MAX : now + timeout_ns;
    return 0;
}

static int64_t lpr_wait_deadline_remaining(
    const lpr_wait_deadline_t *deadline,
    uint64_t *out_remaining)
{
    if (out_remaining == 0) return -LPR_LINUX_EFAULT;
    *out_remaining = UINT64_MAX;
    if (deadline == 0 || !deadline->finite) return 0;
    uint64_t now = 0;
    const int64_t status = lpr_wait_now_ns(&now);
    if (status != 0) return status;
    *out_remaining = deadline->expires_ns > now ?
        deadline->expires_ns - now : 0;
    return 0;
}

int64_t lpr_wait_deadline_expired(
    const lpr_wait_deadline_t *deadline,
    int *out_expired)
{
    if (out_expired == 0) return -LPR_LINUX_EFAULT;
    uint64_t remaining = UINT64_MAX;
    const int64_t status = lpr_wait_deadline_remaining(deadline, &remaining);
    if (status != 0) return status;
    *out_expired = remaining == 0;
    return 0;
}

static int64_t lpr_wait_sleep(uint64_t wait_ns)
{
    if (wait_ns == 0) return 0;
    const struct pachaos_timespec delay = {
        .tv_sec = wait_ns / LPR_WAIT_NS_PER_SEC,
        .tv_nsec = wait_ns % LPR_WAIT_NS_PER_SEC,
    };
    return lpr_pacha_nanosleep(&delay);
}

int64_t lpr_wait_graph_block(
    lpr_wait_graph_t *graph,
    const lpr_wait_deadline_t *deadline)
{
    if (graph == 0) return -LPR_LINUX_EFAULT;
#if defined(LPR_GLYCIN_DIAG) && LPR_GLYCIN_DIAG
    uint32_t diag_watch_index = UINT32_MAX;
    const uint32_t diag_watch_fd = __atomic_load_n(
        &lpr_glycin_diag_socket_fd, __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&lpr_glycin_diag_armed, __ATOMIC_ACQUIRE) != 0u &&
        diag_watch_fd != 0u)
    {
        for (uint32_t i = 0; i < graph->leaf_count; ++i) {
            if (graph->logical_fds[i] == diag_watch_fd) {
                diag_watch_index = i;
                break;
            }
        }
    }
#endif
    uint64_t wait_ns = UINT64_MAX;
    const int64_t deadline_status =
        lpr_wait_deadline_remaining(deadline, &wait_ns);
    if (deadline_status != 0) return deadline_status;
    if (graph->relative_deadline_ns < wait_ns)
        wait_ns = graph->relative_deadline_ns;
    if (wait_ns == 0) return 0;

    uint64_t wait_started_ns = 0;
    if (wait_ns != UINT64_MAX) {
        const int64_t now_status = lpr_wait_now_ns(&wait_started_ns);
        if (now_status != 0) return now_status;
    }

#if defined(LPR_GLYCIN_DIAG) && LPR_GLYCIN_DIAG
    if (0 && diag_watch_index != UINT32_MAX) {
        lpr_glycin_diag_event(
            "wait.block.enter",
            diag_watch_fd,
            (uint64_t)(uint32_t)graph->leaves[diag_watch_index].fd,
            graph->leaf_count,
            wait_ns == UINT64_MAX ? -1 : (int64_t)wait_ns);
    }
#endif

    int64_t status = 0;
    if (graph->leaf_count == 0) {
        if (wait_ns != UINT64_MAX) {
            status = lpr_wait_sleep(wait_ns);
        } else {
            int local_fd = -1;
            int remote_fd = -1;
            status = lpr_native_wait_pair(&local_fd, &remote_fd);
            if (status == 0) {
                struct pacha_pollfd leaf = {
                    .fd = local_fd,
                    .events = PACHA_FD_EVENT_READABLE,
                };
                status = lpr_pacha_syscall4(
                    PACHA_FD_SYSCALL_WAIT_MANY,
                    (uint64_t)(uintptr_t)&leaf,
                    1,
                    PACHA_FD_WAIT_FOREVER,
                    0);
            }
            if (local_fd >= 16)
                (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)local_fd);
            if (remote_fd >= 16)
                (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)remote_fd);
        }
    } else {
        uint64_t timeout_ticks = PACHA_FD_WAIT_FOREVER;
        if (wait_ns != UINT64_MAX) {
            timeout_ticks = wait_ns / LPR_WAIT_NS_PER_MS;
            if (wait_ns % LPR_WAIT_NS_PER_MS != 0) timeout_ticks++;
            if (timeout_ticks == PACHA_FD_WAIT_FOREVER) timeout_ticks--;
        }
        status = lpr_pacha_syscall4(
            PACHA_FD_SYSCALL_WAIT_MANY,
            (uint64_t)(uintptr_t)graph->leaves,
            graph->leaf_count,
            timeout_ticks,
            0);
    }

#if defined(LPR_GLYCIN_DIAG) && LPR_GLYCIN_DIAG
    if (0 && diag_watch_index != UINT32_MAX) {
        lpr_glycin_diag_event(
            "wait.block.exit",
            diag_watch_fd,
            graph->leaves[diag_watch_index].events,
            graph->leaves[diag_watch_index].revents,
            status);
    }
#endif

    if (status >= 0) {
        for (uint32_t i = 0; i < graph->leaf_count; ++i) {
            if ((graph->leaves[i].revents &
                 (PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP)) == 0)
                continue;
            if (graph->drain_modes[i] == LPR_WAIT_DRAIN_EVENT)
                lpr_eventfd_drain_wait(graph->logical_fds[i]);
            else if (graph->drain_modes[i] == LPR_WAIT_DRAIN_NATIVE) {
                if (graph->logical_fds[i] != UINT32_MAX &&
                    lpr_linux_socket_fd_active(graph->logical_fds[i])) {
                    const uint64_t events =
                        lpr_native_wait_drain_events(graph->leaves[i].fd);
                    if (events != 0)
                        lpr_linux_socket_mark_events(
                            graph->logical_fds[i], events);
                    else
                        lpr_linux_socket_mark_readable(graph->logical_fds[i]);
                } else {
                    lpr_native_wait_drain(graph->leaves[i].fd);
                }
            }
        }
    }

#if defined(LPR_GLYCIN_DIAG) && LPR_GLYCIN_DIAG
    if (0 && diag_watch_index != UINT32_MAX) {
        lpr_glycin_diag_event(
            "wait.block.drained",
            diag_watch_fd,
            graph->leaves[diag_watch_index].events,
            graph->leaves[diag_watch_index].revents,
            status);
    }
#endif

    if (lpr_linux_pump_tty_signals())
        return LPR_WAIT_RESTART_SYSCALL;

    /* NOT_READY is also the signal-interrupt resume value.  Unwind even if a
     * concurrent fd event populated revents so the dispatcher can deliver the
     * pending native frame before the atomic wait restores its signal mask. */
    const int retryable_wake =
        status == PACHA_SYSCALL_ERR_NOT_READY ||
         status == -PACHA_SYSCALL_ERR_NOT_READY ||
         status == -LPR_LINUX_EAGAIN;
    if (retryable_wake) {
        int timed_out = 0;
        if (wait_ns != UINT64_MAX) {
            uint64_t now_ns = 0;
            const int64_t now_status = lpr_wait_now_ns(&now_ns);
            if (now_status != 0) return now_status;
            timed_out = now_ns >= wait_started_ns &&
                now_ns - wait_started_ns >= wait_ns;
        }
        return timed_out ? 0 : LPR_WAIT_RESTART_SYSCALL;
    }
    if (status >= 0)
        return 0;
    const int64_t linux_status = lpr_pacha_status_to_errno(status);
    return linux_status == -LPR_LINUX_EAGAIN ? 0 : linux_status;
}
