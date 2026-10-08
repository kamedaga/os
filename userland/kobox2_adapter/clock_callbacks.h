/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_CLOCK_CALLBACKS_H
#define PACHA_KOBOX_CLOCK_CALLBACKS_H

#include "arch/x86_64/fp_entry.h"

/* Linux may call these with its legacy FP bank live. The native syscall
 * retains that bank across signals and scheduling; every C entry and the
 * libpacha syscall leaf are audited as integer-only, including errors.
 * No time cache or independently calibrated counter is introduced. */
/* Read-only kernel clock page, mapped once by ph_machine_init before Linux
 * runs; NULL keeps every read on CLOCK_READ. */
static const volatile unsigned char *clock_page;

#define CLOCK_PAGE_U32(offset) \
    __atomic_load_n((const volatile uint32_t *)(clock_page + (offset)), __ATOMIC_ACQUIRE)
#define CLOCK_PAGE_U64(offset) \
    __atomic_load_n((const volatile uint64_t *)(clock_page + (offset)), __ATOMIC_ACQUIRE)
#define CLOCK_PAGE_PV(field) (PACHA_CLOCK_PAGE_PV0_OFFSET + PACHA_CLOCK_PAGE_PV_##field##_OFFSET)

/* Monotonic time without a syscall, in the kernel's own CLOCK_MONOTONIC domain:
 * the same KVM pvclock record, epoch and conversion as the kernel, bounded by
 * the kernel's ceiling and lifted to its floor. Any doubt (fast closed, odd or
 * torn version, no stable bit, overflow, value above the ceiling) returns 1 so
 * the caller asks the kernel, which may then switch clock source safely.
 * Loads are ordered (x86 load->load and lfence around rdtsc) so that fast is
 * observed as 1 after every other field was read. */
static KOBOX_GPR_LEAF int clock_page_monotonic(uint64_t *nanoseconds) {
    if (!clock_page || CLOCK_PAGE_U32(PACHA_CLOCK_PAGE_FAST_OFFSET) != 1) return 1;
    const uint64_t ceiling = CLOCK_PAGE_U64(PACHA_CLOCK_PAGE_CEILING_NS_OFFSET);
    const uint64_t floor = CLOCK_PAGE_U64(PACHA_CLOCK_PAGE_FLOOR_NS_OFFSET);
    const uint64_t pv_origin = CLOCK_PAGE_U64(PACHA_CLOCK_PAGE_PV_ORIGIN_OFFSET);
    const uint64_t ns_origin = CLOCK_PAGE_U64(PACHA_CLOCK_PAGE_NS_ORIGIN_OFFSET);
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        const uint32_t version = CLOCK_PAGE_U32(CLOCK_PAGE_PV(VERSION));
        if (version & 1) continue;
        const uint64_t stamp = CLOCK_PAGE_U64(CLOCK_PAGE_PV(TSC_TIMESTAMP));
        const uint64_t system_time = CLOCK_PAGE_U64(CLOCK_PAGE_PV(SYSTEM_TIME));
        const uint32_t multiplier = CLOCK_PAGE_U32(CLOCK_PAGE_PV(MULTIPLIER));
        const int8_t shift = (int8_t)clock_page[CLOCK_PAGE_PV(SHIFT)];
        const uint8_t flags = clock_page[CLOCK_PAGE_PV(FLAGS)];
        uint32_t lo, hi;
        __asm__ volatile("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
        if (version != CLOCK_PAGE_U32(CLOCK_PAGE_PV(VERSION))) continue;
        const uint64_t counter = ((uint64_t)hi << 32) | lo;
        if (!(flags & PACHA_CLOCK_PAGE_PV_STABLE_FLAG) || counter < stamp ||
            multiplier == 0 || shift < -63 || shift > 63) return 1;
        /* Same arithmetic as the kernel's pvNanoseconds. A product that might
         * not fit is left to the kernel instead of dividing here. */
        const uint64_t delta = counter - stamp;
        const unsigned __int128 scaled = shift < 0 ? (unsigned __int128)(delta >> -shift) :
            (unsigned __int128)delta << shift;
        if (scaled >> 96) return 1;
        const unsigned __int128 scaled_ns = (scaled * multiplier) >> 32;
        if (scaled_ns > (unsigned __int128)(UINT64_MAX - system_time)) return 1;
        const uint64_t pv = (uint64_t)scaled_ns + system_time;
        if (pv < pv_origin || pv - pv_origin > UINT64_MAX - ns_origin) return 1;
        uint64_t ns = ns_origin + (pv - pv_origin);
        if (ns > ceiling) return 1;
        if (ns < floor) ns = floor;
        if (CLOCK_PAGE_U32(PACHA_CLOCK_PAGE_FAST_OFFSET) != 1) return 1;
        *nanoseconds = ns;
        return 0;
    }
    return 1;
}

static KOBOX_GPR_LEAF int clock_ns(unsigned clock_id, uint64_t *nanoseconds) {
#if defined(PH_CLOCK_TIMESPEC_BENCH)
    uint64_t timespec[2];
    int status = (int)pacha_syscall2(PACHA_RUNTIME_SYSCALL_CLOCK_GETTIME,
        clock_id, (uintptr_t)timespec);
    if (status == 0) *nanoseconds = timespec[0] * 1000000000ull + timespec[1];
    return status;
#else
    return (int)pacha_clock_read(clock_id, nanoseconds);
#endif
}

static KOBOX_GPR_LEAF int monotonic(uint64_t *nanoseconds) {
#if !defined(PH_CLOCK_TIMESPEC_BENCH)
    if (!clock_page_monotonic(nanoseconds)) return 0;
#endif
    return clock_ns(PACHA_TIMERFD_CLOCK_MONOTONIC, nanoseconds);
}

/* Map the kernel clock page read-only. Failure is not an error: reads keep
 * using CLOCK_READ, which is always correct. */
static __attribute__((unused)) void clock_page_map(void) {
#if defined(PH_CLOCK_PAGE_DISABLED)
    return;
#endif
    long fd = pacha_syscall0(PACHA_RUNTIME_SYSCALL_CLOCK_PAGE_OPEN);
    if (fd < 16) return;
    long address = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, (uint64_t)fd, 0, PACHA_CLOCK_PAGE_SIZE,
        PACHA_PROT_READ, PACHA_MMAP_SHARED, 0);
    (void)pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, (uint64_t)fd);
    /* Native status codes are small positive values, never mappable addresses. */
    if (address >= 4096) clock_page = (const volatile unsigned char *)(uintptr_t)address;
}

static KOBOX_GPR_LEAF int realtime(uint64_t *nanoseconds) {
    return clock_ns(0, nanoseconds);
}

#endif
