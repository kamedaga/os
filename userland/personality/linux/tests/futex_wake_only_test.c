#define _GNU_SOURCE
#include "../runtime/lpr_futex_wake.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>
static _Atomic uint32_t word,done;
static int c2,c4;static int64_t reply4;static uint64_t expected_count=1;
static int64_t mock2(uint64_t nr,uint64_t a,uint64_t n){assert(nr==PACHAOS_SYSCALL_FUTEX_WAKE&&a==(uintptr_t)&word&&n==expected_count);c2++;return 0;}
static int64_t mock4(uint64_t nr,uint64_t a,uint64_t n,uint64_t move,uint64_t target){assert(nr==PACHAOS_SYSCALL_FUTEX_REQUEUE&&a==(uintptr_t)&word&&n==1&&!move&&target!=a);c4++;return reply4;}

static pthread_barrier_t barrier;
static void *waiter(void *arg){(void)arg;pthread_barrier_wait(&barrier);struct timespec limit={.tv_sec=2};long rc=syscall(SYS_futex,&word,FUTEX_WAIT_PRIVATE,0,&limit,0,0);assert(rc==0);atomic_fetch_add(&done,1);return 0;}
static void test_pair(int use_requeue){
 atomic_store(&word,0);atomic_store(&done,0);assert(!pthread_barrier_init(&barrier,0,3));pthread_t a,b;assert(!pthread_create(&a,0,waiter,0));assert(!pthread_create(&b,0,waiter,0));pthread_barrier_wait(&barrier);
 long total=0;uint64_t target;assert(lpr_futex_wake_only_target((uintptr_t)&word,1,128,&target));
 for(unsigned i=0;i<200&&total<2;i++){
  /* Harness scheduling only. Production helper has no timer, poll or retry. */
  struct timespec pause={.tv_nsec=1000000};nanosleep(&pause,0);
  long rc=use_requeue?syscall(SYS_futex,&word,FUTEX_REQUEUE_PRIVATE,1,0,(void*)(uintptr_t)target,0):syscall(SYS_futex,&word,FUTEX_WAKE_PRIVATE,1,0,0,0);
  assert(rc==0||rc==1);total+=rc;
 }
 assert(total==2);assert(!pthread_join(a,0)&&!pthread_join(b,0));assert(atomic_load(&done)==2);assert(atomic_load(&word)==0);
 /* move=0 leaves no hidden waiter on the destination. */
 assert(syscall(SYS_futex,(void*)(uintptr_t)target,FUTEX_WAKE_PRIVATE,INT_MAX,0,0,0)==0);assert(!pthread_barrier_destroy(&barrier));
}
int main(void){
 reply4=1;assert(lpr_futex_wake_native((uintptr_t)&word,1,128,mock2,mock4)==1&&c4==1&&c2==0);
 reply4=-PACHAOS_SYSCALL_ERR_INVALID;assert(lpr_futex_wake_native((uintptr_t)&word,1,128,mock2,mock4)==0&&c4==2&&c2==1);
 reply4=-PACHAOS_SYSCALL_ERR_MAP;assert(lpr_futex_wake_native((uintptr_t)&word,1,128,mock2,mock4)==reply4&&c4==3&&c2==1);
 assert(lpr_futex_wake_native((uintptr_t)&word,1,0,mock2,mock4)==0&&c4==3&&c2==2);
 for(unsigned i=0;i<3;i++){expected_count=i==0?0:i==1?2:UINT_MAX;assert(lpr_futex_wake_native((uintptr_t)&word,expected_count,128,mock2,mock4)==0&&c4==3);}expected_count=1;
 uint64_t t=0,other=0;assert(lpr_futex_wake_only_target((uintptr_t)&word,1,128,&t));assert(t&&(t&3)==0&&t!=(uintptr_t)&word);
 assert(lpr_futex_wake_only_target(t,1,128,&other)&&other!=t);
 assert(!lpr_futex_wake_only_target(0,1,128,&t));assert(!lpr_futex_wake_only_target(5,1,128,&t));assert(!lpr_futex_wake_only_target((uintptr_t)&word,0,128,&t));assert(!lpr_futex_wake_only_target((uintptr_t)&word,2,128,&t));assert(!lpr_futex_wake_only_target((uintptr_t)&word,UINT_MAX,128,&t));assert(!lpr_futex_wake_only_target((uintptr_t)&word,1,0,&t));assert(!lpr_futex_wake_only_target((uintptr_t)&word,1,384,&t));
 void *page=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(page!=MAP_FAILED);
 assert(lpr_futex_wake_only_target((uintptr_t)page,1,128,&t));
 for(unsigned i=0;i<2;i++){assert(!mprotect(page,4096,i?PROT_NONE:PROT_READ));errno=0;long a=syscall(SYS_futex,page,FUTEX_WAKE_PRIVATE,1,0,0,0);int ea=errno;errno=0;long b=syscall(SYS_futex,page,FUTEX_REQUEUE_PRIVATE,1,0,(void*)(uintptr_t)t,0);assert(a==b&&(a>=0||ea==errno));}assert(!munmap(page,4096));
 for(unsigned i=0;i<50;i++){test_pair(0);test_pair(1);}puts("PASS: native plan boundaries/alias, Linux WAKE vs zero-move REQUEUE, exactly one per call, no stranded/migrated waiter or word mutation (100 pairs)");
}
