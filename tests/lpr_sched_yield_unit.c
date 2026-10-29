#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "../userland/personality/linux/runtime/lpr_dispatch.c"

static unsigned calls;
static int64_t native_result;
int lpr_linux_thread_exists(uint64_t tid) { return tid==0 || tid==17; }
int lpr_user_range_plausible(uint64_t ptr,uint64_t size)
{ return ptr>=4096 && ptr<=INT64_MAX && size<=INT64_MAX-ptr; }
void *lpr_memcpy(void *dst,const void *src,size_t size) { return memcpy(dst,src,size); }

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
    assert(lpr_sys_sched_yield(0, 0, 0, 0, 0, 0, 0) == 0);
    assert(calls == 1 && errno == EAGAIN);
    native_result = PACHA_SYSCALL_ERR_ALLOC;
    assert(lpr_sys_sched_yield(0, 0, 0, 0, 0, 0, 0) == -ENOMEM);
    assert(calls == 2 && errno == EAGAIN);
    _Static_assert(LPR_LINUX_SYS_SCHED_GET_PRIORITY_MAX == 146, "Linux x86_64 priority max");
    _Static_assert(LPR_LINUX_SYS_SCHED_GET_PRIORITY_MIN == 147, "Linux x86_64 priority min");
    for (unsigned policy=0;policy<10;++policy) {
        int expected_min,expected_max;
        if (policy==1 || policy==2) { expected_min=1;expected_max=99; }
        else if (policy==0 || policy==3 || policy==5 || policy==6 || policy==7)
            expected_min=expected_max=0;
        else expected_min=expected_max=-EINVAL;
        assert(lpr_sys_sched_get_priority_min(policy,0,0,0,0,0,0)==expected_min);
        assert(lpr_sys_sched_get_priority_max(policy,0,0,0,0,0,0)==expected_max);
    }
    assert(lpr_sys_sched_get_priority_min(UINT64_MAX,0,0,0,0,0,0)==-EINVAL);
    assert(lpr_sys_sched_get_priority_max(1ull<<32,0,0,0,0,0,0)==0);
    _Static_assert(LPR_LINUX_SYS_SCHED_SETSCHEDULER==144, "Linux x86_64 scheduler");
    int32_t priority=0;
    const uint64_t param=(uintptr_t)&priority;
    assert(!lpr_sys_sched_setscheduler(17,0,param,0,0,0,0));
    assert(!lpr_sys_sched_setparam(0,param,0,0,0,0,0));
    priority=123;
    assert(!lpr_sys_sched_getparam(17,param,0,0,0,0,0) && priority==0);
    assert(!lpr_sys_sched_getscheduler(17,0,0,0,0,0,0));
    priority=1;
    assert(lpr_sys_sched_setscheduler(17,0,param,0,0,0,0)==-EINVAL);
    assert(lpr_sys_sched_setscheduler(17,1,param,0,0,0,0)==-EOPNOTSUPP);
    assert(lpr_sys_sched_setscheduler(17,4,param,0,0,0,0)==-EINVAL);
    priority=0;
    assert(lpr_sys_sched_setscheduler(18,0,param,0,0,0,0)==-ESRCH);
    assert(lpr_sys_sched_getparam(18,param,0,0,0,0,0)==-ESRCH);
    assert(lpr_sys_sched_getscheduler(UINT64_MAX,0,0,0,0,0,0)==-EINVAL);
    assert(lpr_sys_sched_setscheduler(17,0,0,0,0,0,0)==-EINVAL);
    assert(lpr_sys_sched_getparam(17,UINT64_MAX,0,0,0,0,0)==-EFAULT);
    assert(!lpr_sys_sched_getscheduler(17,0,0,0,0,0,0));
    puts("sched_yield native forwarding PASS");
    puts("sched_get_priority_min/max Linux policy bounds PASS");
    puts("normal scheduler reapply/query, invalid target/arguments and unsupported classes PASS");
    return 0;
}
