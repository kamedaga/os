#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "../userland/personality/linux/runtime/lpr_mlock.c"

static unsigned allocations,populates;
static int fail_allocate,fail_populate,fail_unmaps;
void lpr_state_lock(volatile uint32_t *word) { assert(!*word);*word=1; }
void lpr_state_unlock(volatile uint32_t *word) { assert(*word);*word=0; }
void *lpr_memcpy(void *d,const void *s,size_t n) { return memcpy(d,s,n); }
void *lpr_memmove(void *d,const void *s,size_t n) { return memmove(d,s,n); }
int64_t lpr_pacha_syscall6(uint64_t nr,uint64_t fd,uint64_t addr,uint64_t bytes,
    uint64_t prot,uint64_t flags,uint64_t offset)
{
    (void)fd;(void)addr;(void)prot;(void)flags;(void)offset;
    assert(nr==PACHAOS_SYSCALL_MMAP);
    if (fail_allocate) return PACHA_SYSCALL_ERR_ALLOC;
    void *memory=calloc(1,bytes);assert(memory);++allocations;return (uintptr_t)memory;
}
int64_t lpr_pacha_syscall2(uint64_t nr,uint64_t addr,uint64_t bytes)
{
    assert(nr==PACHAOS_SYSCALL_MUNMAP && bytes && allocations);
    if (fail_unmaps) { --fail_unmaps;return PACHA_SYSCALL_ERR_MAP; }
    free((void *)(uintptr_t)addr);--allocations;return 0;
}
int64_t lpr_pacha_syscall1(uint64_t nr,uint64_t code)
{
    assert(nr==PACHAOS_SYSCALL_PROCESS_EXIT && code==127);
    _exit((int)code);
}
int64_t lpr_pacha_syscall3(uint64_t nr,uint64_t addr,uint64_t size,uint64_t advice)
{
    assert(nr==PACHA_VM_SYSCALL_MADVISE && !(addr&4095) && !(size&4095));
    assert(advice==0 || advice==PACHA_VM_MADV_POPULATE_WRITE);
    if (advice) { ++populates;if (fail_populate) return PACHA_SYSCALL_ERR_MAP; }
    return 0;
}
int main(void)
{
    assert(lpr_linux_mlock(0x1000,4096,0)==-LPR_LINUX_EPERM);
    assert(!allocations && !populates);
    assert(!lpr_linux_mlock(0x1000,4096,8192));
    assert(!lpr_linux_mlock(0x1001,4095,8192));
    assert(!lpr_linux_mlock(0x2000,4096,8192));
    unsigned before=populates;
    assert(lpr_linux_mlock(0x4000,4096,8192)==-LPR_LINUX_ENOMEM && populates==before);
    assert(!lpr_linux_mlock(0x3000,4096,12288));
    assert(!lpr_linux_munlock(0x2000,4096));
    assert(!lpr_linux_mlock(0x5000,4096,12288));
    assert(lpr_linux_mlock(0x6000,4096,12288)==-LPR_LINUX_ENOMEM);
    assert(!lpr_mlock_vm_begin());lpr_mlock_forget(0x1000,4096);lpr_mlock_vm_end();
    assert(!lpr_linux_mlock(0x6000,4096,12288));
    fail_populate=1;
    assert(lpr_linux_mlock(0x9000,4096,16384)==-LPR_LINUX_ENOMEM);
    fail_populate=0;
    assert(!lpr_linux_mlock(0xa000,4096,16384));
    lpr_mlock_fork_lock();lpr_mlock_after_fork_child();
    assert(!allocations && !locked.lock && !locked.count);
    fail_allocate=1;
    assert(lpr_linux_mlock(0x1000,4096,4096)==-LPR_LINUX_ENOMEM);
    fail_allocate=0;
    assert(!lpr_linux_mlock(0x1000,4096,4096));
    assert(!lpr_linux_munlock(0x1000,4096) && !allocations);
    assert(!lpr_linux_mlock(0x1000,4096,4096));
    locked_range_t *old=locked.ranges;
    fail_unmaps=1;
    assert(reserve(locked.capacity+1)==-LPR_LINUX_ENOMEM);
    assert(locked.ranges==old && allocations==1 && locked.count==1);
    pid_t child=fork();assert(child>=0);
    if (!child) { fail_unmaps=2;reserve(locked.capacity+1);_exit(99); }
    int child_status;assert(waitpid(child,&child_status,0)==child);
    assert(WIFEXITED(child_status) && WEXITSTATUS(child_status)==127);
    fail_unmaps=1;
    assert(!lpr_linux_munlock(0x1000,4096));
    assert(allocations==1 && locked.ranges==old && !locked.count);
    assert(!lpr_linux_mlock(0x2000,4096,4096));
    assert(locked.ranges==old && allocations==1);
    assert(!lpr_linux_munlock(0x2000,4096) && !allocations);
    assert(lpr_linux_mlock(UINT64_MAX,4096,UINT64_MAX)==-LPR_LINUX_EINVAL);
    puts("mlock limits/overlap/split/unmap/fork and failed prefault/allocation PASS");
}
