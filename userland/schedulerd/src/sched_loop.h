#ifndef PACHA_SCHEDULERD_SCHED_LOOP_H
#define PACHA_SCHEDULERD_SCHED_LOOP_H

#include "pacha/scheduler_abi.h"
#include "pacha_sched.h"

#include <stdint.h>

#define PACHA_SCHED_LOOP_MAX_CPUS 64u
#define PACHA_SCHED_LOOP_EVENT_BUCKETS 8u

typedef struct pacha_sched_loop {
    pacha_sched_state sched;
    pacha_sched_decision decision;
    pacha_eevdf_pick_result pick_scratch;
    pacha_eevdf_runqueue runqueue_scratch;
    int schedctl_fd;
    int event_fd;
    uint32_t cpu_count;
    uint64_t dispatch_count;
    uint64_t commit_count;
    uint64_t metric_read_count;
    uint64_t metric_read_cycles;
    uint64_t metric_read_max_cycles;
    uint64_t metric_dispatch_cycles;
    uint64_t metric_dispatch_max_cycles;
    uint64_t metric_pick_count;
    uint64_t metric_pick_cycles;
    uint64_t metric_pick_max_cycles;
    uint64_t metric_commit_cycles;
    uint64_t metric_commit_max_cycles;
    uint64_t metric_event_type_count[PACHA_SCHED_LOOP_EVENT_BUCKETS];
    uint64_t metric_dispatch_type_cycles[PACHA_SCHED_LOOP_EVENT_BUCKETS];
    uint64_t metric_dispatch_type_max_cycles[PACHA_SCHED_LOOP_EVENT_BUCKETS];
    uint64_t metric_next_report;
} pacha_sched_loop_t;

void pacha_sched_loop_init(pacha_sched_loop_t *loop, int schedctl_fd, int event_fd);
int pacha_sched_loop_dispatch(pacha_sched_loop_t *loop, const pacha_sched_event_t *event);
int pacha_sched_loop_run_once(pacha_sched_loop_t *loop);
int pacha_sched_loop_demo(pacha_sched_loop_t *loop);

#endif
