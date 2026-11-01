#include <assert.h>
#include <stdint.h>
#include <stdio.h>

struct pacha_native_result;
static struct pacha_native_result invoke(uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t, uint64_t, uint64_t);
#define PACHA_NATIVE_SYSCALL_TEST_INVOKE invoke
#include "pacha/native_syscall_retry.h"

static uint8_t supplied[2] = {1, 2};
static unsigned calls;
static uint64_t deadline;
static long completion;

static struct pacha_native_result invoke(uint64_t nr, uint64_t word,
    uint64_t operation, uint64_t deadline_ms, uint64_t flags,
    uint64_t unused0, uint64_t unused1)
{
    assert(nr == PACHA_RUNTIME_SYSCALL_FUTEX_PI && word == 0x8000);
    assert(operation == 0 && flags == 1 && !unused0 && !unused1);
    /* Each absent reply replaces output RDX with an address. Every retry
     * must nevertheless restore the original input deadline, including 0. */
    assert(deadline_ms == deadline);
    if (calls++ < 2)
        return (struct pacha_native_result){PACHA_SYSCALL_ERR_PAGER_ABSENT,
            (uintptr_t)&supplied[calls - 1]};
    return (struct pacha_native_result){completion, 0xdead};
}

int main(void)
{
    for (unsigned infinite = 0; infinite < 2; ++infinite) {
        deadline = infinite ? 0 : 123456;
        /* A native timeout is a terminal result, not another pager retry.
         * This tests transport only; runtime decides timeout vs handoff. */
        for (completion = 0; completion <= 5; completion += 5) {
            calls = 0;
            const struct pacha_native_result result = pacha_native_syscall_retry6(
                PACHA_RUNTIME_SYSCALL_FUTEX_PI, 0x8000, 0, deadline, 1, 0, 0);
            assert(calls == 3 && result.status == completion && result.rdx == 0xdead);
        }
    }
    puts("PI pager retry: original absolute deadline and key flags preserved PASS");
}
