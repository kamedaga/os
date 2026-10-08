/* SPDX-License-Identifier: MIT */
/* Reuse the full-bank assembly invocation, not compiler-lived XMM values. */
#define main notification_contract_main
#include "kobox2_notification_fp_unit.c"
#undef main

static uint64_t clock_output;
static unsigned clock_calls, nested;
static int clock_error;
static int mono_call(uint64_t argument);

#ifdef TEST_NATIVE
static KOBOX_GPR_LEAF long clock_mock(uint64_t id, uint64_t *out)
{
    ++clock_calls;
    /* A native signal/upcall protects its entry even if its C workload
     * touches FP. This mock is not proof of actual kernel signal return. */
    if (nested) {
        nested = 0;
        ph_notifications_dispatch_preserved();
    }
    *out = clock_error ? 0 : UINT64_C(1000000000) + id;
    return clock_error;
}
#define pacha_clock_read clock_mock
#include "../userland/kobox2_adapter/clock_callbacks.h"
#undef pacha_clock_read
static KOBOX_GPR_LEAF int mono_call(uint64_t arg) { return monotonic((void *)(uintptr_t)arg); }
static KOBOX_GPR_LEAF int real_call(uint64_t arg) { return realtime((void *)(uintptr_t)arg); }
#else
#include <time.h>
static bool actual_clock;
static int clock_mock(clockid_t id, struct timespec *out);
#define clock_gettime clock_mock
#include "../kobox2/linux-sandbox/kobox/host/posix/clock.c"
#undef clock_gettime
static int mono_call(uint64_t arg) { return kobox_posix_monotonic_ns((void *)(uintptr_t)arg); }
static int real_call(uint64_t arg) { return kobox_posix_realtime_ns((void *)(uintptr_t)arg); }

static int clock_mock(clockid_t id, struct timespec *out)
{
    ++clock_calls;
    struct fp_bank entry;
    asm volatile("fxsaveq %0" : "=m"(entry) : : "memory");
    assert(entry.bytes[0] == 0x7f && entry.bytes[1] == 3 && !entry.bytes[4]);
    assert(entry.bytes[24] == 0x80 && entry.bytes[25] == 0x1f);
    /* The mock forces error/reentry/clobber paths; the actual provider checks
     * that the same entry contract surrounds libc's real clock implementation. */
    if (actual_clock)
        return clock_gettime(id, out);
    if (nested) {
        nested = 0;
        uint64_t inner;
        assert(!checked_call(mono_call, (uintptr_t)&inner, &nested_seed));
        assert(inner == UINT64_C(2000000003));
        /* This is a real host signal delivery inside the protected libc
         * interval. The handler intentionally clobbers its FP bank. */
        assert(!raise(SIGRTMIN));
    }
    *out = (struct timespec){.tv_sec = clock_error == EOVERFLOW ? -1 : 2, .tv_nsec = 3};
    errno = clock_error;
    asm volatile("fxrstorq %0" : : "m"(clobber) : "memory");
    if (clock_error == EOVERFLOW) return 0;
    (void)id;
    return clock_error ? -1 : 0;
}
#endif

int main(void)
{
    assert(!notification_contract_main());
    clock_calls = 0;
    clock_output = UINT64_MAX;
    assert(!checked_call(mono_call, (uintptr_t)&clock_output, &seed));
#ifdef TEST_NATIVE
    assert(clock_output == UINT64_C(1000000001));
#else
    assert(clock_output == UINT64_C(2000000003));
    unsigned previous = clock_calls;
    assert(checked_call(mono_call, 0, &seed) == EINVAL && clock_calls == previous);
    assert(checked_call(real_call, 0, &seed) == EINVAL && clock_calls == previous);
#endif
    assert(!checked_call(real_call, (uintptr_t)&clock_output, &seed));
    clock_output = UINT64_MAX;
    clock_error = EIO;
    assert(checked_call(mono_call, (uintptr_t)&clock_output, &seed) == EIO);
#ifdef TEST_NATIVE
    assert(clock_output == 0);
#else
    assert(clock_output == UINT64_MAX);
    clock_error = EOVERFLOW;
    assert(checked_call(real_call, (uintptr_t)&clock_output, &seed) == EOVERFLOW);
    assert(clock_output == UINT64_MAX);
#endif
    clock_error = 0;
    nested = 1;
    assert(!checked_call(mono_call, (uintptr_t)&clock_output, &seed));
    assert(!nested);
#ifndef TEST_NATIVE
    actual_clock = true;
    clock_output = 0;
    assert(!checked_call(mono_call, (uintptr_t)&clock_output, &seed));
    assert(clock_output != 0);
    clock_output = 0;
    assert(!checked_call(real_call, (uintptr_t)&clock_output, &seed));
    assert(clock_output != 0);
#endif
    puts("KOBOX2_CLOCK_FP_UNIT_OK full bank, errors, nested notification/libc");
    return 0;
}
