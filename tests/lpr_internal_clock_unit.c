#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <pacha/status.h>

struct pacha_native_result;
static struct pacha_native_result invoke(uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t, uint64_t, uint64_t);
#define PACHA_NATIVE_SYSCALL_TEST_INVOKE invoke
#include "../userland/personality/linux/runtime/lpr_vfs/ops.c"

static unsigned calls, transient;
static uint64_t expected_clock, nanoseconds;
static long failure;

static struct pacha_native_result invoke(uint64_t nr, uint64_t clock,
    uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    assert(nr == PACHAOS_SYSCALL_CLOCK_READ && clock == expected_clock);
    assert(!(a1 | a2 | a3 | a4 | a5));
    ++calls;
    if (transient) {
        long status = transient-- == 2 ? PACHA_SYSCALL_ERR_NOT_READY :
            -PACHA_SYSCALL_ERR_NOT_READY;
        return (struct pacha_native_result){status, UINT64_MAX};
    }
    return (struct pacha_native_result){failure, nanoseconds};
}

void *lpr_memset(void *p, int value, size_t size) { return memset(p, value, size); }
int64_t lpr_pacha_status_to_errno(int64_t status)
{
    return pacha_kernel_status_to_errno(status);
}

int main(void)
{
    struct pachaos_timespec out;
    const uint64_t values[] = {0, 999999999, 1000000000,
        UINT64_C(1793031000123456789), UINT64_MAX};
    for (expected_clock = 0; expected_clock <= 1; ++expected_clock) {
        for (size_t i = 0; i < sizeof(values)/sizeof(*values); ++i) {
            nanoseconds = values[i]; calls = 0; transient = 2;
            assert(!lpr_pacha_clock_gettime(expected_clock, &out));
            assert(calls == 3);
            assert(out.tv_sec == nanoseconds / UINT64_C(1000000000));
            assert(out.tv_nsec == nanoseconds % UINT64_C(1000000000));
        }
    }
    expected_clock = 99; failure = PACHA_SYSCALL_ERR_INVALID;
    out = (struct pachaos_timespec){123, 456};
    assert(lpr_pacha_clock_gettime(expected_clock, &out) ==
        pacha_kernel_status_to_errno(failure));
    assert(!out.tv_sec && !out.tv_nsec);
    puts("LPR internal scalar clock: PASS");
}
