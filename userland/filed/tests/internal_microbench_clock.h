/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
extern int filed_bench_collect;
extern uint64_t filed_bench_exchange_cycles, filed_bench_exchange_count;
extern int filed_bench_prefault;
extern uint64_t filed_bench_snapshot_cycles[10], filed_bench_storage_cycles[4];
extern uint64_t filed_bench_channel_cycles[3];
#ifdef FILED_INTERNAL_RING_ONLY
extern uint64_t filed_bench_wait_cycles[2], filed_bench_wait_counts[3];
#endif
static inline uint64_t filed_bench_tsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}
/* Nested read/exchange counters are inclusive; snapshot stages partition the
 * whole operation. Keeping clocks diagnostic-only avoids production overhead. */
#define FILED_BENCH_STAGE(array, index, start) do { \
    if (filed_bench_collect) { \
        uint64_t end = filed_bench_tsc(); \
        (array)[index] += end - (start); \
        (start) = end; \
    } \
} while (0)
