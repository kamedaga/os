#pragma once

/* Diagnostic only. The caller's existing DRM mapping lock serializes record
 * and fork copies; emit runs after unlocking. Cumulative snapshots stop after
 * 16 records, one per 65536 calls. Intervals include descheduling, not CPU time.
 * Unlock and the accounting itself are outside the measured intervals. */
#if defined(LPR_UNMAP_PROFILE) && LPR_UNMAP_PROFILE
typedef struct lpr_unmap_profile_snapshot {
    /* count, <=8KiB, failures, invalid clocks, lock wait, native, lease work,
     * first timestamp, latest completion timestamp */
    uint64_t value[9];
} lpr_unmap_profile_snapshot_t;

static lpr_unmap_profile_snapshot_t lpr_unmap_profile_totals;

static inline uint64_t lpr_unmap_profile_clock(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

static inline void lpr_unmap_profile_reset(void)
{
    lpr_unmap_profile_totals = (lpr_unmap_profile_snapshot_t){0};
}

static inline int lpr_unmap_profile_record(uint64_t length, int64_t result,
    const uint64_t stamp[4], lpr_unmap_profile_snapshot_t *snapshot)
{
    uint64_t *v = lpr_unmap_profile_totals.value;
    if (!v[0]) v[7] = stamp[0];
    ++v[0];
    v[1] += length != 0 && length <= 8192;
    v[2] += result != 0;
    if (stamp[0] <= stamp[1] && stamp[1] <= stamp[2] && stamp[2] <= stamp[3]) {
        v[4] += stamp[1] - stamp[0];
        v[5] += stamp[2] - stamp[1];
        v[6] += stamp[3] - stamp[2];
    } else ++v[3];
    v[8] = stamp[3];
    if ((v[0] & 65535u) != 0 || v[0] > 16u * 65536u) return 0;
    *snapshot = lpr_unmap_profile_totals;
    return 1;
}

static inline void lpr_unmap_profile_emit(const lpr_unmap_profile_snapshot_t *snapshot)
{
    char line[208];
    unsigned n = 0;
    const char *prefix = "LPR_UNMAP_STAGE";
    while (*prefix) line[n++] = *prefix++;
    for (unsigned i = 0; i < 10; ++i) {
        uint64_t value = i ? snapshot->value[i - 1] : (uint64_t)lpr_linux_current_pid;
        line[n++] = ' ';
        for (int shift = 60; shift >= 0; shift -= 4)
            line[n++] = "0123456789abcdef"[(value >> shift) & 15];
    }
    line[n++] = '\n';
    (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_LOG, (uint64_t)(uintptr_t)line, n);
}
#endif
