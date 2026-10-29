/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "../userland/personality/linux/runtime/lpr_vfs/control_cache.c"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned creates, closes, maps, unmaps;
static int fail_create, fail_map, fail_unmap;
void lpr_state_lock(volatile uint32_t *word) { (void)word; assert(!pthread_mutex_lock(&mutex)); }
void lpr_state_unlock(volatile uint32_t *word) { (void)word; assert(!pthread_mutex_unlock(&mutex)); }
void *lpr_memset(void *p, int c, size_t n) { return memset(p,c,n); }
int64_t lpr_pacha_status_to_errno(int64_t status) { return status; }
int64_t lpr_pacha_syscall1(uint64_t nr,uint64_t fd)
{ assert(nr==PACHAOS_SYSCALL_FD_CLOSE && fd>=16);closes++;return 0; }
int64_t lpr_pacha_syscall2(uint64_t nr,uint64_t address,uint64_t bytes)
{
    assert(nr==PACHAOS_SYSCALL_MUNMAP);
    if(fail_unmap)return -LPR_LINUX_EIO;
    assert(!munmap((void*)(uintptr_t)address,bytes));unmaps++;return 0;
}
int64_t lpr_pacha_syscall3(uint64_t nr,uint64_t bytes,uint64_t rights,uint64_t flags)
{
    assert(nr==PACHAOS_SYSCALL_VMO_CREATE && bytes);
    assert(rights&PACHA_FD_RIGHT_TRANSFER);
    assert(flags==(PACHA_FD_FLAG_PRIVATE|PACHA_FD_FLAG_CLOEXEC));
    if(fail_create)return -LPR_LINUX_ENOMEM;
    return 100+creates++;
}
int64_t lpr_pacha_syscall6(uint64_t nr,uint64_t fd,uint64_t offset,uint64_t bytes,uint64_t prot,uint64_t flags,uint64_t address)
{
    assert(nr==PACHAOS_SYSCALL_MMAP && fd>=16 && !offset && !address);
    assert(prot==(PACHAOS_PROT_READ|PACHAOS_PROT_WRITE) && flags==PACHAOS_MMAP_SHARED);
    if(fail_map)return -LPR_LINUX_ENOMEM;
    void*p=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    assert(p!=MAP_FAILED);maps++;return (intptr_t)p;
}
static pthread_barrier_t gate;
static uint64_t concurrent_addresses[8];
static void *concurrent(void *argument)
{
    unsigned index=(unsigned)(uintptr_t)argument;
    struct lpr_fs_control_lease lease;
    assert(!lpr_fs_control_acquire(&lease,4096));
    concurrent_addresses[index]=lease.address;
    int rc=pthread_barrier_wait(&gate);assert(!rc||rc==PTHREAD_BARRIER_SERIAL_THREAD);
    for(unsigned i=0;i<8;i++)if(i!=index)assert(concurrent_addresses[index]!=concurrent_addresses[i]);
    rc=pthread_barrier_wait(&gate);assert(!rc||rc==PTHREAD_BARRIER_SERIAL_THREAD);
    lpr_fs_control_release(&lease,1);
    return NULL;
}
static void drop_idle(void)
{ for(unsigned i=0;i<4;i++)destroy(&idle[i]); }
int main(void)
{
    struct lpr_fs_control_lease a,b;
    uint64_t first=0;
    for(unsigned i=0;i<100;i++){
        assert(!lpr_fs_control_acquire(&a,4096));
        if(!i)first=a.address;else assert(a.address==first);
        for(unsigned j=0;j<4096;j++)assert(!((unsigned char*)(uintptr_t)a.address)[j]);
        memset((void*)(uintptr_t)a.address,0xa5,4096);
        lpr_fs_control_release(&a,1);
    }
    assert(creates==1&&maps==1&&!closes&&!unmaps);
    /* Concurrent/reentrant requests must never share a live scratch page. */
    assert(!lpr_fs_control_acquire(&a,4096));
    assert(!lpr_fs_control_acquire(&b,4096)&&a.address!=b.address);
    unsigned old_creates=creates,old_closes=closes;
    lpr_fs_control_release(&a,0); /* An uncertain response retires the object. */
    assert(closes==old_closes+1);
    assert(!lpr_fs_control_acquire(&a,4096)&&creates==old_creates+1);
    lpr_fs_control_release(&a,1);lpr_fs_control_release(&b,1);
    struct lpr_fs_control_lease leases[9];
    for(unsigned i=0;i<9;i++){
        assert(!lpr_fs_control_acquire(&leases[i],4096));
        for(unsigned j=0;j<i;j++)assert(leases[j].address!=leases[i].address);
    }
    for(unsigned i=0;i<9;i++)lpr_fs_control_release(&leases[i],1);
    assert(!pthread_barrier_init(&gate,NULL,8));
    pthread_t threads[8];
    for(unsigned i=0;i<8;i++)assert(!pthread_create(&threads[i],NULL,concurrent,(void*)(uintptr_t)i));
    for(unsigned i=0;i<8;i++)assert(!pthread_join(threads[i],NULL));
    assert(!pthread_barrier_destroy(&gate));
    assert(maps-unmaps==4); /* Retained inventory stays bounded. */
    old_creates=creates;old_closes=closes;
    assert(!lpr_fs_control_acquire(&a,8192));lpr_fs_control_release(&a,1);
    assert(creates==old_creates+1&&closes==old_closes+1);
    /* Fork drops both idle and in-flight mappings, never copied FD numbers. */
    assert(!lpr_fs_control_acquire(&a,4096));
    ((unsigned char*)(uintptr_t)a.address)[0]=0x5a;
    pid_t child=fork();assert(child>=0);
    if(!child){unsigned before=closes;assert(!lpr_fs_control_fork_child());assert(!active&&closes==before);for(unsigned i=0;i<4;i++)assert(!idle[i].address);_exit(0);}
    int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
    assert(((unsigned char*)(uintptr_t)a.address)[0]==0x5a);
    fail_unmap=1;assert(lpr_fs_control_fork_child()==-LPR_LINUX_EIO);fail_unmap=0;
    lpr_fs_control_release(&a,1);drop_idle();
    fail_create=1;assert(lpr_fs_control_acquire(&a,4096)==-LPR_LINUX_ENOMEM);fail_create=0;
    old_closes=closes;fail_map=1;assert(lpr_fs_control_acquire(&a,4096)==-LPR_LINUX_ENOMEM);fail_map=0;assert(closes==old_closes+1);
    assert(!active&&maps==unmaps&&creates==closes);
    puts("FS control scratch: reuse, isolation, uncertain completion, bounded retention, fork and allocation failures PASS");
}
