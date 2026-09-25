#pragma once

#include <stdint.h>
#include <time.h>

/* Wall-time diagnostics otherwise issue native clock syscalls around every
 * request. Keep operation/error counts and TSC totals in normal builds;
 * explicitly opt in when nanosecond diagnostics are needed. This clock must
 * never be used for deadlines or filesystem timestamps. */
#ifndef FILED_WALL_TIME_METRICS
#define FILED_WALL_TIME_METRICS 0
#endif

static inline uint64_t filed_metric_now_ns(void)
{
#if FILED_WALL_TIME_METRICS
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#else
    return 0;
#endif
}
