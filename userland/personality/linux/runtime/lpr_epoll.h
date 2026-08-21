#ifndef LPR_EPOLL_H
#define LPR_EPOLL_H

#include "lpr_wait.h"

#include <stdint.h>

int lpr_linux_epoll_fd_active(uint64_t fd);
int64_t lpr_linux_epoll_create1(uint64_t flags);
int64_t lpr_linux_epoll_ctl(uint64_t epfd, uint64_t op, uint64_t fd, uint64_t event);
int64_t lpr_linux_epoll_wait(uint64_t epfd, uint64_t events, uint64_t maxevents, uint64_t timeout);
int64_t lpr_linux_epoll_pwait(
    uint64_t epfd,
    uint64_t events,
    uint64_t maxevents,
    uint64_t timeout,
    uint64_t sigmask,
    uint64_t sigsetsize);
int64_t lpr_epoll_poll_events(uint64_t fd, uint32_t events);
int64_t lpr_epoll_add_wait_graph(uint64_t fd, lpr_wait_graph_t *graph);
void lpr_epoll_note_fd_state(uint64_t fd);
void lpr_epoll_before_close(uint64_t fd);

#endif
