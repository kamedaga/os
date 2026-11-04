/* SPDX-License-Identifier: MIT */
#ifndef GPUD_READY_H
#define GPUD_READY_H
#include <pacha/ipc.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
/* A collector only observes native FD readiness. It never touches DRM state,
 * transferred capabilities, the implicit reply, or the sandbox GPU channel. */
struct gpud_ready;
struct gpud_ready_source;
struct gpud_ready_event {
    struct gpud_ready_source *source;
    void *context;
    uint64_t events, generation;
    unsigned kind;
};
struct gpud_ready *gpud_ready_create(void);
int gpud_ready_destroy(struct gpud_ready *);
pacha_fd_t gpud_ready_fd(struct gpud_ready *);
int gpud_ready_add(struct gpud_ready *, pacha_fd_t, uint64_t, unsigned, void *,
                   struct gpud_ready_source **);
int gpud_ready_remove(struct gpud_ready_source **);
int gpud_ready_rearm(struct gpud_ready_source *);
/* Nonblocking O(1) when no source changed. Delivery disarms that source until
 * rearm/remove; the queue is bounded by registrations, not producer traffic. */
int gpud_ready_next(struct gpud_ready *, struct gpud_ready_event *);
int gpud_ready_error(struct gpud_ready *);
/* OS boundary, also used by the pthread host fixture. Native worker stacks
 * have no libc TLS and the worker calls only these nonallocating operations. */
struct gpud_ready_worker;
int gpud_ready_os_event(pacha_fd_t *);
int gpud_ready_os_signal(pacha_fd_t);
int gpud_ready_os_drain(pacha_fd_t);
int gpud_ready_os_wait(struct pacha_fd_wait_retained *, struct pacha_pollfd *, size_t, uint64_t);
int gpud_ready_os_start(struct gpud_ready_worker *, void (*)(struct gpud_ready_worker *), void **);
int gpud_ready_os_join(void *);
int gpud_ready_os_ack(void *, pacha_fd_t);
int gpud_ready_os_close(pacha_fd_t);
void gpud_ready_worker_run(struct gpud_ready_worker *);
#endif
