#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include "../userland/personality/linux/runtime/lpr_dispatch.c"

static unsigned calls;
static int64_t native_result;

int64_t lpr_pacha_syscall0(uint64_t nr)
{
    assert(nr == PACHA_RUNTIME_SYSCALL_YIELD);
    calls++;
    return native_result;
}

int main(void)
{
    _Static_assert(LPR_LINUX_SYS_SCHED_YIELD == 24, "Linux ABI stays unchanged");
    _Static_assert(PACHA_RUNTIME_SYSCALL_YIELD == PACHAOS_SYSCALL_YIELD,
                   "native ABI headers must agree");
    errno = EAGAIN;
    assert(lpr_sys_sched_yield(0, 0, 0, 0, 0, 0) == 0);
    assert(calls == 1 && errno == EAGAIN);
    native_result = PACHA_SYSCALL_ERR_ALLOC;
    assert(lpr_sys_sched_yield(0, 0, 0, 0, 0, 0) == -ENOMEM);
    assert(calls == 2 && errno == EAGAIN);
    puts("sched_yield native forwarding PASS");
    return 0;
}
