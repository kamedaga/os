/* SPDX-License-Identifier: MIT */
#ifndef LIVE_DESKTOP_FRAME_PROFILE_H
#define LIVE_DESKTOP_FRAME_PROFILE_H
#include <stdint.h>
enum { FRAME_PHASE_DRAW, FRAME_PHASE_SUBMIT, FRAME_PHASE_GPU_WAIT,
       FRAME_PHASE_SWAP, FRAME_PHASE_PRESENT, FRAME_PHASE_RETIRE,
       FRAME_PHASE_COUNT, FRAME_TRACE_SLOTS = 48, FRAME_POLL_SLOTS = 96 };
struct frame_trace_entry {
    uint32_t phase, operation;
    uint64_t calls, cycles, maximum, errors;
};
struct frame_trace_snapshot {
    struct frame_trace_entry entries[FRAME_TRACE_SLOTS];
    uint64_t overflow;
    struct frame_trace_poll_entry {
        uint32_t phase, nfds, events, revents;
        int32_t fd, timeout;
        uint64_t calls, cycles, maximum, ready, timeouts, errors;
    } polls[FRAME_POLL_SLOTS];
    uint64_t poll_overflow;
};
/* Test-local interposer interface, not a PachaOS or Kobox2 wire interface. */
void frame_trace_phase(unsigned phase);
void frame_trace_reset(void);
void frame_trace_snapshot(struct frame_trace_snapshot *out);
static inline uint64_t frame_cycles(void)
{
    uint32_t low, high;
    __asm__ volatile("lfence; rdtsc" : "=a"(low), "=d"(high) :: "memory");
    return ((uint64_t)high << 32) | low;
}
#endif
