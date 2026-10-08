/* SPDX-License-Identifier: MIT */
#pragma once
#ifdef PH_FS_CHANNEL_PROFILE
#include <stdint.h>
static inline uint64_t ph_fs_channel_profile_tsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}
/* Diagnostic-only partitions. Waiting includes the peer and its downstream
 * storage hop; it is not a measurement of native IPC syscall cost alone. */
extern void ph_fs_channel_profile(uint32_t opcode, const uint64_t cycles[4],
    unsigned notifications, unsigned waits);
#ifdef PH_FS_CHANNEL_TRACE
/* Correlate private diagnostic logs, never append timestamps to the wire.
 * publish is sampled before release publication; completion after acquire. */
extern void ph_fs_channel_trace(uint64_t channel, uint32_t opcode, uint64_t sequence,
    uint64_t publish, uint64_t completion);
#endif
#endif
