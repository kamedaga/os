#ifndef PACHA_SCHEDULERD_SCHED_LOOP_H
#define PACHA_SCHEDULERD_SCHED_LOOP_H

#include "eevdf.h"
#include "pacha/scheduler_abi.h"

#include <stdint.h>

#define PACHA_SCHED_LOOP_MAX_CPUS 64u

typedef struct pacha_sched_loop {
    pacha_eevdf_runqueue_t runqueues[PACHA_SCHED_LOOP_MAX_CPUS];
    int schedctl_fd;
    int event_fd;
    uint64_t known_cpu_mask;
    uint64_t idle_cpu_mask;
    uint64_t running_thread[PACHA_SCHED_LOOP_MAX_CPUS];
    uint64_t running_generation[PACHA_SCHED_LOOP_MAX_CPUS];
    uint64_t dispatch_count;
    uint64_t commit_count;
} pacha_sched_loop_t;

void pacha_sched_loop_init(pacha_sched_loop_t *loop, int schedctl_fd, int event_fd);
int pacha_sched_loop_dispatch(pacha_sched_loop_t *loop, const pacha_sched_event_t *event);
int pacha_sched_loop_run_once(pacha_sched_loop_t *loop);
int pacha_sched_loop_demo(pacha_sched_loop_t *loop);

#endif
