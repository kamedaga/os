#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../userland/personality/linux/runtime/lpr_pipe/io.c"

static lpr_pipe_backend_t pipe_state = {.native = {.raw = 40}};
static uint64_t space, input_events;
static int peer_closed, calls, native_error, drm_active;
lpr_pipe_backend_t *lpr_pipe_backend(uint64_t fd) { return fd == 3 ? &pipe_state : NULL; }
void *lpr_memset(void *p, int v, size_t n) { return memset(p, v, n); }
/* Non-pipe backends are outside this focused wait-graph fixture. */
#define INACTIVE(name) int name(uint64_t fd) { (void)fd; return 0; }
INACTIVE(lpr_linux_tty_fd_active)
int lpr_linux_drm_fd_active(uint64_t fd) { return drm_active && fd == 5; }
int lpr_drm_native_wait_fd(uint64_t fd) { return fd == 5 ? 41 : -1; }
INACTIVE(lpr_unix_socket_active)
INACTIVE(lpr_linux_input_fd_active)
INACTIVE(lpr_linux_sync_file_fd_active)
INACTIVE(lpr_linux_epoll_fd_active)
INACTIVE(lpr_linux_socket_fd_active)
INACTIVE(lpr_linux_inotify_active)
INACTIVE(lpr_linux_timerfd_active)
INACTIVE(lpr_linux_signalfd_active)
INACTIVE(lpr_linux_filed_fd_active)
INACTIVE(lpr_linux_device_fd_active)
lpr_event_backend_t *lpr_event_backend(uint64_t fd) { (void)fd; return NULL; }
#include "../userland/personality/linux/runtime/lpr_wait.c"
int64_t lpr_pacha_syscall4(uint64_t nr, uint64_t raw, uint64_t count,
    uint64_t timeout, uint64_t flags)
{
    assert(nr == PACHA_FD_SYSCALL_WAIT_MANY && count == 1 && !timeout && !flags);
    struct pacha_pollfd *p = (void *)(uintptr_t)raw;
    assert(p->fd == 40 && p->revents == LPR_LINUX_PIPE_BUF_BYTES);
    if (native_error) return PACHA_SYSCALL_ERR_INVALID;
    uint64_t ready = input_events;
    if (space >= p->revents && !peer_closed) ready |= PACHA_FD_EVENT_WRITABLE;
    if (peer_closed) ready |= PACHA_FD_EVENT_ERROR | PACHA_FD_EVENT_HANGUP;
    p->revents = ready; ++calls;
    return ready ? 1 : PACHA_SYSCALL_ERR_NOT_READY;
}
int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t raw, uint64_t count)
{
    assert(nr == PACHAOS_SYSCALL_FD_POLL && count == 1);
    struct pacha_pollfd *p = (void *)(uintptr_t)raw;
    assert(p->fd == 40 && p->revents == 0);
    p->revents = input_events; ++calls;
    return input_events ? 1 : 0;
}
int main(void)
{
    for (space = 0; space < LPR_LINUX_PIPE_BUF_BYTES; ++space)
        assert(lpr_linux_pipe_poll_events(3, 4) == 0);
    assert(lpr_linux_pipe_poll_events(3, 4) == 4);
    space = 1; input_events = PACHA_FD_EVENT_READABLE;
    assert(lpr_linux_pipe_poll_events(3, 5) == 1); /* drain, don't block writing */
    assert(lpr_linux_pipe_poll_events(3, 1) == 1);
    input_events = 0; assert(lpr_linux_pipe_poll_events(3, 1) == 0);
    peer_closed = 1; assert(lpr_linux_pipe_poll_events(3, 4) == (8 | 16));
    native_error = 1; assert(lpr_linux_pipe_poll_events(3, 4) == 32);
    native_error = 0;
    int before = calls; assert(lpr_linux_pipe_poll_events(99, 4) == 32);
    assert(calls == before);
    lpr_wait_graph_t graph;
    lpr_wait_graph_init(&graph);
    assert(lpr_wait_graph_add_fd(&graph, 3, 4) == 0);
    assert(graph.leaf_count == 1 && graph.leaves[0].fd == 40);
    assert(graph.leaves[0].revents == LPR_LINUX_PIPE_BUF_BYTES);
    lpr_wait_graph_init(&graph);
    assert(lpr_wait_graph_add_fd(&graph, 3, 1) == 0);
    assert(graph.leaves[0].revents == 0);
    /* A specific blocking write still waits for its actual atomic size. */
    lpr_wait_graph_init(&graph);
    assert(lpr_wait_graph_add_native_min(&graph, 40, PACHA_FD_EVENT_WRITABLE, 58) == 0);
    assert(graph.leaves[0].revents == 58);
    drm_active = 1;
    lpr_wait_graph_init(&graph);
    assert(lpr_wait_graph_add_fd(&graph, 5, 1) == 0);
    assert(graph.leaf_count == 1 && graph.leaves[0].fd == 41);
    assert(graph.leaves[0].events ==
        (PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP));
    assert(graph.relative_deadline_ns == UINT64_MAX);
    puts("LPR_PIPE_POLL_UNIT=OK");
    return 0;
}
