#pragma once

/* Opt-in, bounded elapsed-stage diagnostic. TSC intervals include scheduling;
 * they are not CPU time. Never enable this in the ordinary packaged runtime. */
#if defined(LPR_MMAP_PROFILE) && LPR_MMAP_PROFILE
static inline uint64_t lpr_map_profile_clock(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return ((uint64_t)hi << 32) | lo;
}

static inline void lpr_map_profile_emit(const char *kind, uint64_t fd,
    uint64_t length, uint64_t info, const uint64_t stamp[6])
{
    static uint32_t reports;
    if (stamp[5] < stamp[0] || stamp[5] - stamp[0] < 100000000ull)
        return;
    if (__atomic_fetch_add(&reports, 1u, __ATOMIC_RELAXED) >= 48u)
        return;
    char line[320];
    unsigned n = 0;
    const char *prefix = "LPR_MAP_STAGE ";
    while (*prefix) line[n++] = *prefix++;
    for (unsigned i = 0; kind[i] && i < 8; ++i) line[n++] = kind[i];
    const uint64_t values[] = {
        (uint64_t)lpr_linux_current_pid, fd, length, info,
        stamp[0], stamp[1], stamp[2], stamp[3], stamp[4], stamp[5],
    };
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        line[n++] = ' ';
        for (int shift = 60; shift >= 0; shift -= 4)
            line[n++] = "0123456789abcdef"[(values[i] >> shift) & 15];
    }
    line[n++] = '\n';
    (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_LOG,
        (uint64_t)(uintptr_t)line, n);
}
#else
#define lpr_map_profile_clock() 0ull
#define lpr_map_profile_emit(kind, fd, length, info, stamp) ((void)(stamp))
#endif
