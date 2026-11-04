#include "lpr_filed_internal.h"
#include "lpr_clock.h"
#include "lpr_clock_entry.h"

_Static_assert(offsetof(lpr_signal_thread_state_t, native_pending_hint) == LPR_CLOCK_HINT_OFFSET, "clock entry signal hint");
_Static_assert(offsetof(lpr_signal_thread_state_t, native_pending_hint_active) == LPR_CLOCK_HINT_ACTIVE_OFFSET, "clock entry signal registration");
_Static_assert(offsetof(lpr_signal_thread_state_t, pending_mask) == LPR_CLOCK_PENDING_OFFSET, "clock entry pending mask");
/* Private assembly inputs, not exported process ABI. The count pointer avoids
 * duplicating lpr_state's layout or maintaining a second thread counter. */
const volatile uint32_t *const lpr_clock_thread_count = &lpr_state.thread_count;
const volatile unsigned char *lpr_clock_fast_page;

static const volatile unsigned char *clock_page;
/* 0=unattempted, 1=initializer, 2=done (including permanent map failure).
 * Contenders fall back instead of waiting, so reentrancy cannot deadlock.
 * The read-only shared mapping lasts for this image and is inherited on fork. */
static uint32_t clock_mapping_state;
static volatile uint32_t clock_mapping_lock;

static void clock_page_map(void)
{
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&clock_mapping_lock, &expected, 1, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    if (__atomic_load_n(&clock_mapping_state, __ATOMIC_RELAXED) == 2) {
        lpr_state_unlock(&clock_mapping_lock);
        return;
    }
    __atomic_store_n(&clock_mapping_state, 1, __ATOMIC_RELAXED);
    int64_t fd = lpr_pacha_syscall0(PACHAOS_SYSCALL_CLOCK_PAGE_OPEN);
    if (fd >= 16) {
        int64_t address = lpr_pacha_syscall6(PACHAOS_SYSCALL_MMAP,
            fd, 0, PACHA_CLOCK_PAGE_SIZE, PACHAOS_PROT_READ, PACHAOS_MMAP_SHARED, 0);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, fd);
        if (address >= 4096) {
            clock_page = (const volatile unsigned char *)(uintptr_t)address;
            __atomic_store_n(&lpr_clock_fast_page, clock_page, __ATOMIC_RELEASE);
        }
    }
    __atomic_store_n(&clock_mapping_state, 2, __ATOMIC_RELEASE);
    lpr_state_unlock(&clock_mapping_lock);
}

void lpr_clock_fork_lock(void) { lpr_state_lock(&clock_mapping_lock); }
void lpr_clock_fork_unlock(void) { lpr_state_unlock(&clock_mapping_lock); }

void lpr_clock_after_fork_child(void)
{
    /* The fork lock excludes a half-published mmap from the VM snapshot, so
     * every inherited page has a recorded owner. Drop the inherited lock;
     * a completed mapping remains valid and read-only in this image. */
    clock_mapping_lock = 0;
    if (clock_mapping_state == 1) {
        clock_page = 0;
        __atomic_store_n(&lpr_clock_fast_page, 0, __ATOMIC_RELEASE);
        clock_mapping_state = 0;
    }
}

#define PAGE_U32(offset) __atomic_load_n((const volatile uint32_t *)(page + (offset)), __ATOMIC_ACQUIRE)
#define PAGE_U64(offset) __atomic_load_n((const volatile uint64_t *)(page + (offset)), __ATOMIC_ACQUIRE)
#define PV(field) (PACHA_CLOCK_PAGE_PV0_OFFSET + PACHA_CLOCK_PAGE_PV_##field##_OFFSET)

#ifndef LPR_CLOCK_COUNTER
static uint64_t clock_counter(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}
#define LPR_CLOCK_COUNTER() clock_counter()
#endif

int lpr_clock_monotonic(uint64_t *nanoseconds)
{
    if (__atomic_load_n(&clock_mapping_state, __ATOMIC_ACQUIRE) != 2) clock_page_map();
    if (__atomic_load_n(&clock_mapping_state, __ATOMIC_ACQUIRE) != 2) return 1;
    const volatile unsigned char *page = clock_page;
    if (!page || PAGE_U32(PACHA_CLOCK_PAGE_FAST_OFFSET) != 1) return 1;
    const uint64_t ceiling = PAGE_U64(PACHA_CLOCK_PAGE_CEILING_NS_OFFSET);
    const uint64_t floor = PAGE_U64(PACHA_CLOCK_PAGE_FLOOR_NS_OFFSET);
    const uint64_t pv_origin = PAGE_U64(PACHA_CLOCK_PAGE_PV_ORIGIN_OFFSET);
    const uint64_t ns_origin = PAGE_U64(PACHA_CLOCK_PAGE_NS_ORIGIN_OFFSET);
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        const uint32_t version = PAGE_U32(PV(VERSION));
        if (version & 1) continue;
        const uint64_t stamp = PAGE_U64(PV(TSC_TIMESTAMP));
        const uint64_t system_time = PAGE_U64(PV(SYSTEM_TIME));
        const uint32_t multiplier = PAGE_U32(PV(MULTIPLIER));
        const int8_t shift = (int8_t)page[PV(SHIFT)];
        const uint8_t flags = page[PV(FLAGS)];
        const uint64_t counter = LPR_CLOCK_COUNTER();
        if (version != PAGE_U32(PV(VERSION))) continue;
        if (!(flags & PACHA_CLOCK_PAGE_PV_STABLE_FLAG) || counter < stamp ||
            !multiplier || shift < -63 || shift > 63) return 1;
        /* Match kobox2_adapter/clock_callbacks.h: reject before a potentially
         * overflowing product, then verify both epoch additions. There is no
         * independent time cache or frequency calibration in this reader. */
        const uint64_t delta = counter - stamp;
        const unsigned __int128 scaled = shift < 0 ?
            (unsigned __int128)(delta >> -shift) : (unsigned __int128)delta << shift;
        if (scaled >> 96) return 1;
        const unsigned __int128 scaled_ns = (scaled * multiplier) >> 32;
        if (scaled_ns > (unsigned __int128)(UINT64_MAX - system_time)) return 1;
        const uint64_t pv = (uint64_t)scaled_ns + system_time;
        if (pv < pv_origin || pv - pv_origin > UINT64_MAX - ns_origin) return 1;
        uint64_t ns = ns_origin + (pv - pv_origin);
        if (ns > ceiling) return 1;
        if (ns < floor) ns = floor;
        if (PAGE_U32(PACHA_CLOCK_PAGE_FAST_OFFSET) != 1) return 1;
        *nanoseconds = ns;
        return 0;
    }
    return 1;
}

extern unsigned char lpr_clock_copy_seconds[], lpr_clock_copy_nanos[], lpr_clock_copy_bad[];
extern unsigned char lpr_clock_entry_copy_seconds[], lpr_clock_entry_copy_nanos[], lpr_clock_entry_copy_bad[];
uintptr_t lpr_clock_copy_fault_target(uint64_t vector, uint64_t error, uint64_t rip)
{
    /* Only the audited timespec stores may recover. Other LPR faults
     * retain their normal signal handling; COW faults are resolved natively. */
    if (vector == 14 && (error & 2) && !(error & 16) &&
        (rip == (uintptr_t)lpr_clock_copy_seconds || rip == (uintptr_t)lpr_clock_copy_nanos))
        return (uintptr_t)lpr_clock_copy_bad;
    if (vector == 14 && (error & 2) && !(error & 16) &&
        (rip == (uintptr_t)lpr_clock_entry_copy_seconds || rip == (uintptr_t)lpr_clock_entry_copy_nanos))
        return (uintptr_t)lpr_clock_entry_copy_bad;
    return 0;
}
