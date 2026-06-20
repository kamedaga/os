#ifndef PACHA_SCHEDULER_ABI_H
#define PACHA_SCHEDULER_ABI_H

#include <stdint.h>

#define PACHA_SCHED_ABI_VERSION 1u

#define PACHA_FD_KIND_SCHEDCTL 15ull
#define PACHA_FD_KIND_SCHED_EVENT 16ull

#define PACHA_SCHED_EVENT_THREAD_READY 1u
#define PACHA_SCHED_EVENT_THREAD_BLOCKED 2u
#define PACHA_SCHED_EVENT_THREAD_EXITED 3u
#define PACHA_SCHED_EVENT_THREAD_YIELD 4u
#define PACHA_SCHED_EVENT_TICK 5u
#define PACHA_SCHED_EVENT_CPU_IDLE 6u

#define PACHA_SCHED_FLAG_NONE 0ull
#define PACHA_SCHED_FLAG_PREEMPTED (1ull << 0)
#define PACHA_SCHED_FLAG_BOOTSTRAP (1ull << 1)

#define PACHA_SCHED_IOCTL_QUERY_CAPS 0x53434801ull
#define PACHA_SCHED_IOCTL_COMMIT 0x53434802ull
#define PACHA_SCHED_IOCTL_SET_WEIGHT 0x53434803ull

#define PACHA_SCHED_NO_THREAD 0ull

typedef struct pacha_sched_event {
    uint32_t size;
    uint16_t version;
    uint16_t type;
    uint64_t sequence;
    uint32_t cpu_id;
    uint32_t _reserved0;
    uint64_t thread_id;
    uint64_t generation;
    uint64_t runtime_ns;
    uint64_t weight;
    uint64_t slice_ns;
} pacha_sched_event_t;

typedef struct pacha_sched_commit {
    uint32_t size;
    uint16_t version;
    uint16_t _reserved0;
    uint32_t cpu_id;
    uint32_t flags;
    uint64_t thread_id;
    uint64_t generation;
    uint64_t sequence;
} pacha_sched_commit_t;

typedef struct pacha_sched_weight {
    uint32_t size;
    uint16_t version;
    uint16_t _reserved0;
    uint64_t thread_id;
    uint64_t generation;
    uint64_t weight;
    uint64_t slice_ns;
} pacha_sched_weight_t;

typedef struct pacha_sched_caps {
    uint32_t size;
    uint16_t version;
    uint16_t _reserved0;
    uint64_t event_size;
    uint64_t commit_size;
    uint64_t weight_size;
    uint64_t flags;
} pacha_sched_caps_t;

#endif
