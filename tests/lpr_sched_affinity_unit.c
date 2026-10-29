#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "../userland/personality/linux/runtime/lpr_dispatch.c"
static uint64_t cpus;
static int64_t info_error;
int lpr_linux_thread_exists(uint64_t tid) { return tid == 0 || tid == 17; }
int lpr_user_range_plausible(uint64_t p, uint64_t n) { return p >= 4096 && p <= INT64_MAX && n <= INT64_MAX-p; }
void *lpr_memcpy(void *d,const void *s,size_t n) { return memcpy(d,s,n); }
int64_t lpr_pacha_syscall1(uint64_t nr,uint64_t out)
{
    assert(nr == PACHAOS_SYSCALL_SYSTEM_INFO);
    ((pachaos_system_info_t *)(uintptr_t)out)->online_cpu_count=cpus;
    return info_error;
}
int main(void)
{
    const uint64_t cases[]={1,4,24,64,65,130};
    uint64_t mask[4];
    for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        cpus=cases[i];memset(mask,0xa5,sizeof(mask));
        uint64_t bytes=((cpus+63)/64)*8;
        assert(lpr_dispatch_sched_getaffinity(17,sizeof(mask),(uintptr_t)mask)==(int64_t)bytes);
        for (unsigned bit=0;bit<bytes*8;bit++) assert(((mask[bit/64]>>(bit%64))&1)==(bit<cpus));
        assert(mask[bytes/8]==UINT64_C(0xa5a5a5a5a5a5a5a5));
        if (bytes>8) assert(lpr_dispatch_sched_getaffinity(17,bytes-1,(uintptr_t)mask)==-EINVAL);
    }
    cpus=24;
    assert(lpr_dispatch_sched_getaffinity(0,8,(uintptr_t)mask)==8 && mask[0]==0xffffff);
    assert(lpr_dispatch_sched_getaffinity(18,8,(uintptr_t)mask)==-ESRCH);
    assert(lpr_dispatch_sched_getaffinity(0,7,(uintptr_t)mask)==-EINVAL);
    assert(lpr_dispatch_sched_getaffinity(0,8,0)==-EFAULT);
    assert(lpr_dispatch_sched_getaffinity(0,8,UINT64_MAX)==-EFAULT);
    mask[0]=123;cpus=0;
    assert(lpr_dispatch_sched_getaffinity(0,8,(uintptr_t)mask)==-EIO && mask[0]==123);
    info_error=PACHA_SYSCALL_ERR_ALLOC;
    assert(lpr_dispatch_sched_getaffinity(0,8,(uintptr_t)mask)==-ENOMEM && mask[0]==123);
    puts("sched_getaffinity native online topology, 24 CPUs, multiword masks and failures PASS");
}
