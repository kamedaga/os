/* SPDX-License-Identifier: MIT */
/* Temporary x86_64 elapsed-span probe. Include only in diagnostic builds.
 * Rows may overlap; these are response times, not exclusive CPU usage.
 * PROFILE_LAYER and PROFILE_DUMP_DELAY identify/stagger each translation unit.
 */
#include <stdint.h>
#include <pacha/syscall.h>

static uint64_t stage_now(void) {
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

static void stage_record(unsigned row, uint64_t start, uint64_t end) {
    static struct { uint64_t count, cycles; } rows[32];
    static unsigned dumped;
    const uint64_t begin = UINT64_C(310000000000);
    const uint64_t finish = UINT64_C(354000000000);
    if (row >= 32 || start < begin || end < start)
        return;
    if (end < finish) {
        __atomic_fetch_add(&rows[row].count, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&rows[row].cycles, end - start, __ATOMIC_RELAXED);
        return;
    }
    if (end < finish + PROFILE_DUMP_DELAY ||
        __atomic_exchange_n(&dumped, 1, __ATOMIC_RELAXED))
        return;
    char output[4096], *p = output;
    for (unsigned i = 0; i < 32; ++i) {
        uint64_t count = __atomic_load_n(&rows[i].count, __ATOMIC_RELAXED);
        if (!count)
            continue;
        const char *label = "[gpu-stage] " PROFILE_LAYER;
        while (*label)
            *p++ = *label++;
        uint64_t values[] = {i, count,
            __atomic_load_n(&rows[i].cycles, __ATOMIC_RELAXED)};
        for (unsigned n = 0; n < 3; ++n) {
            char digits[20];
            unsigned used = 0;
            uint64_t v = values[n];
            *p++ = ' ';
            do { digits[used++] = (char)('0' + v % 10); v /= 10; } while (v);
            while (used)
                *p++ = digits[--used];
        }
        *p++ = '\n';
    }
    (void)pacha_syscall2(1, (uintptr_t)output, (uintptr_t)(p - output));
}
