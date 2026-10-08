/* SPDX-License-Identifier: MIT */
/* The adapter's clock page reader against a hand-built page: same conversion
 * as the kernel, ceiling/floor bounds, and CLOCK_READ for every doubtful case. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pacha/abi.h>
#include <pacha/syscall.h>
#include "arch/x86_64/fp_entry.h"

static unsigned syscall_reads;
#define SYSCALL_VALUE UINT64_C(0x5ca11ed)
static long clock_read_mock(uint64_t id, uint64_t *out)
{
    (void)id;
    ++syscall_reads;
    *out = SYSCALL_VALUE;
    return 0;
}
#define pacha_clock_read clock_read_mock
#include "../userland/kobox2_adapter/clock_callbacks.h"
#undef pacha_clock_read

/* Native status codes are small positive values: OPEN yields an FD and MMAP
 * fails with PACHA_SYSCALL_ERR_MAP, which must not become a page pointer. */
static long map_result = PACHA_SYSCALL_ERR_MAP;
static unsigned closes;
long pacha_syscall0(uint64_t nr) { return nr == PACHA_RUNTIME_SYSCALL_CLOCK_PAGE_OPEN ? 20 : -1; }
long pacha_syscall1(uint64_t nr, uint64_t a) { closes += nr == PACHA_FD_SYSCALL_CLOSE && a == 20; return 0; }
long pacha_syscall6(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f)
{ (void)nr; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; return map_result; }

static unsigned char page[4096] __attribute__((aligned(4096)));

static void put32(uint64_t offset, uint32_t value) { memcpy(page + offset, &value, 4); }
static void put64(uint64_t offset, uint64_t value) { memcpy(page + offset, &value, 8); }
static uint64_t tsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

/* Independent restatement of kernel/src/realtime_clock.zig pvNanoseconds. */
static uint64_t expected(uint64_t counter, uint64_t stamp, uint32_t mul, int shift,
    uint64_t system_time, uint64_t pv_origin, uint64_t ns_origin)
{
    unsigned __int128 delta = counter - stamp;
    delta = shift < 0 ? delta >> -shift : delta << shift;
    uint64_t pv = (uint64_t)((delta * mul) >> 32) + system_time;
    return ns_origin + (pv - pv_origin);
}

enum { MUL = 0x80000000u, SHIFT = -1 }; /* 0.25 ns per TSC tick */
static uint64_t stamp;

static void build(uint32_t fast, uint32_t version, uint8_t flags, uint64_t ceiling, uint64_t floor)
{
    memset(page, 0, sizeof(page));
    stamp = tsc() - 1000;
    put32(PACHA_CLOCK_PAGE_FAST_OFFSET, fast);
    put64(PACHA_CLOCK_PAGE_PV_ORIGIN_OFFSET, 100);
    put64(PACHA_CLOCK_PAGE_NS_ORIGIN_OFFSET, 5000);
    put64(PACHA_CLOCK_PAGE_FLOOR_NS_OFFSET, floor);
    put64(PACHA_CLOCK_PAGE_CEILING_NS_OFFSET, ceiling);
    const uint64_t pv = PACHA_CLOCK_PAGE_PV0_OFFSET;
    put32(pv + PACHA_CLOCK_PAGE_PV_VERSION_OFFSET, version);
    put64(pv + PACHA_CLOCK_PAGE_PV_TSC_TIMESTAMP_OFFSET, stamp);
    put64(pv + PACHA_CLOCK_PAGE_PV_SYSTEM_TIME_OFFSET, 100);
    put32(pv + PACHA_CLOCK_PAGE_PV_MULTIPLIER_OFFSET, MUL);
    page[pv + PACHA_CLOCK_PAGE_PV_SHIFT_OFFSET] = (unsigned char)(int8_t)SHIFT;
    page[pv + PACHA_CLOCK_PAGE_PV_FLAGS_OFFSET] = flags;
}

static uint64_t read_once(void)
{
    (void)realtime;
    uint64_t value = 0;
    assert(!monotonic(&value));
    return value;
}

int main(void)
{
    const uint64_t far = UINT64_MAX / 2;
    /* A failed map leaves no page, closes the FD, and keeps CLOCK_READ. */
    clock_page = NULL;
    clock_page_map();
    assert(clock_page == NULL && closes == 1);
    for (long status = PACHA_SYSCALL_ERR_INVALID; status <= PACHA_SYSCALL_ERR_CLOSED; ++status) {
        map_result = status;
        clock_page_map();
        assert(clock_page == NULL);
    }
    map_result = (long)(uintptr_t)page;
    clock_page_map();
    assert(clock_page == page);
    /* No page: always CLOCK_READ. */
    clock_page = NULL;
    syscall_reads = 0;
    assert(read_once() == SYSCALL_VALUE && syscall_reads == 1);

    clock_page = page;
    /* Valid stable page: bracketed by the same conversion before/after. */
    build(1, 2, 1, far, 0);
    syscall_reads = 0;
    uint64_t before = expected(tsc(), stamp, MUL, SHIFT, 100, 100, 5000);
    uint64_t value = read_once();
    uint64_t after = expected(tsc(), stamp, MUL, SHIFT, 100, 100, 5000);
    assert(syscall_reads == 0 && before <= value && value <= after);

    /* Floor lifts a smaller conversion. */
    build(1, 2, 1, far, far - 1);
    syscall_reads = 0;
    assert(read_once() == far - 1 && syscall_reads == 0);

    /* Every doubtful case asks the kernel. */
    struct { uint32_t fast, version; uint8_t flags; uint64_t ceiling; const char *name; } cases[] = {
        {0, 2, 1, far, "fast closed"},
        {1, 3, 1, far, "odd version"},
        {1, 2, 0, far, "no stable bit"},
        {1, 2, 1, 5000, "above ceiling"},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        build(cases[i].fast, cases[i].version, cases[i].flags, cases[i].ceiling, 0);
        syscall_reads = 0;
        if (read_once() != SYSCALL_VALUE || syscall_reads != 1) {
            fprintf(stderr, "case %s did not fall back\n", cases[i].name);
            return 1;
        }
    }
    /* Counter behind the stamp, a product that might overflow, and a value
     * before the epoch origin are left to the kernel as well. */
    build(1, 2, 1, far, 0);
    put64(PACHA_CLOCK_PAGE_PV0_OFFSET + PACHA_CLOCK_PAGE_PV_TSC_TIMESTAMP_OFFSET, UINT64_MAX);
    syscall_reads = 0;
    assert(read_once() == SYSCALL_VALUE && syscall_reads == 1);
    build(1, 2, 1, far, 0);
    page[PACHA_CLOCK_PAGE_PV0_OFFSET + PACHA_CLOCK_PAGE_PV_SHIFT_OFFSET] = 63;
    syscall_reads = 0;
    assert(read_once() == SYSCALL_VALUE && syscall_reads == 1);
    build(1, 2, 1, far, 0);
    put64(PACHA_CLOCK_PAGE_PV_ORIGIN_OFFSET, UINT64_MAX);
    syscall_reads = 0;
    assert(read_once() == SYSCALL_VALUE && syscall_reads == 1);
    build(1, 2, 1, far, 0);
    put32(PACHA_CLOCK_PAGE_PV0_OFFSET + PACHA_CLOCK_PAGE_PV_MULTIPLIER_OFFSET, 0);
    syscall_reads = 0;
    assert(read_once() == SYSCALL_VALUE && syscall_reads == 1);
    puts("KOBOX2_CLOCK_PAGE_UNIT=PASS");
    return 0;
}
