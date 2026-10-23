#include <assert.h>
#include <stdint.h>

struct pacha_native_result;
static struct pacha_native_result fake_syscall(
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
#define PACHA_NATIVE_SYSCALL_TEST_INVOKE fake_syscall
#include "pacha/native_syscall_retry.h"

static int calls;
static uint8_t pager_byte = 0x5a;
static struct pacha_native_result fake_syscall(
    uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t a3, uint64_t a4, uint64_t a5)
{
    assert(nr == 91 && a0 == 1 && a1 == 2 && a2 == 3 &&
        a3 == 4 && a4 == 5 && a5 == 6);
    if (++calls == 1)
        return (struct pacha_native_result){PACHA_SYSCALL_ERR_PAGER_ABSENT,
            (uintptr_t)&pager_byte};
    return (struct pacha_native_result){17, 23};
}

int main(void)
{
    const struct pacha_native_result result = pacha_native_syscall_retry6(
        91, 1, 2, 3, 4, 5, 6);
    assert(calls == 2 && result.status == 17 && result.rdx == 23);
    return 0;
}
