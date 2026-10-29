#include "lpr_ipc_mock.h"
/* Real wrapper contention paths; raw syscall boundary completes each wait. */
#include <assert.h>
#include <stdio.h>
#include <setjmp.h>
#include <string.h>
#include <stdlib.h>
#include "../userland/personality/linux/runtime/lpr_filed_internal.h"
#if INTERNAL_WAIT_MODE == 1
#include "../userland/personality/linux/runtime/lpr_block/client.c"
#elif INTERNAL_WAIT_MODE == 2
#include "../userland/personality/linux/runtime/lpr_common/runtime_support.c"
#include "../userland/personality/linux/runtime/lpr_fd/table.c"
#elif INTERNAL_WAIT_MODE == 3
#include "../userland/personality/linux/runtime/lpr_fd/control.c"
#elif INTERNAL_WAIT_MODE == 4
#include "../userland/personality/linux/runtime/lpr_socket.c"
#elif INTERNAL_WAIT_MODE == 7
#include "../userland/personality/linux/runtime/lpr_process/syscalls.c"
#elif INTERNAL_WAIT_MODE == 6
#include "../userland/personality/linux/runtime/lpr_vfs/session.c"
int64_t lpr_wait_native_retained_fds(struct pacha_pollfd *leaves,
    uint32_t count, uint64_t timeout, uint64_t native_flags)
{
 assert(count==2 && native_flags==PACHA_WAIT_UNINTERRUPTIBLE);
 return lpr_pacha_syscall4(PACHA_FD_SYSCALL_WAIT_MANY,
     (uint64_t)(uintptr_t)leaves, count, timeout, native_flags);
}
#endif
lpr_state_t lpr_state;
static unsigned waits, sends;
static int force_committed;
#if INTERNAL_WAIT_MODE == 7
static unsigned live_launches, released_reservations;
static int fail_launch_stage;
#endif
int64_t lpr_pacha_syscall4(uint64_t n,uint64_t a,uint64_t b,uint64_t c,uint64_t flags)
{
 (void)b;(void)c;
#if INTERNAL_WAIT_MODE == 7
 if(n==PACHAOS_SYSCALL_FD_DUP)return 42;
#endif
 assert(flags==PACHA_WAIT_UNINTERRUPTIBLE); ++waits;
 if(n==PACHA_RUNTIME_SYSCALL_FUTEX_WAIT){ *(volatile uint32_t *)(uintptr_t)a=INTERNAL_WAIT_MODE==7?1:0;return 0; }
 if(n==PACHA_FD_SYSCALL_WAIT_MANY){ ((struct pacha_pollfd *)(uintptr_t)a)->revents=PACHA_FD_EVENT_READABLE|PACHA_FD_EVENT_WRITABLE;return 1; }
 assert(n==PACHAOS_SYSCALL_IPC_RECV_WAIT); return 0;
}
int64_t lpr_pacha_syscall3(uint64_t n,uint64_t a,uint64_t b,uint64_t c)
{ (void)a;(void)b; assert(n==PACHAOS_SYSCALL_IPC_NOTIFY_WAIT && c==PACHA_WAIT_UNINTERRUPTIBLE);++waits;return 0; }
int64_t lpr_pacha_syscall2(uint64_t n,uint64_t a,uint64_t b)
{
#if INTERNAL_WAIT_MODE == 7
 if(n==PACHAOS_SYSCALL_MUNMAP) {
  assert(b==LPR_THREAD_LAUNCH_BYTES && live_launches);
  --live_launches;free((void *)(uintptr_t)a);return 0;
 }
#endif
 (void)a;(void)b; assert(n!=PACHAOS_SYSCALL_IPC_CALL); if(n==PACHAOS_SYSCALL_IPC_SEND) return sends++==0?PACHA_SYSCALL_ERR_NOT_READY:0;return 0;
}
#if INTERNAL_WAIT_MODE != 2
int64_t lpr_pacha_status_to_errno(int64_t s) { return pacha_kernel_status_to_errno(s); }
#endif
#if INTERNAL_WAIT_MODE == 7
void lpr_wait_thread_release(lpr_thread_record_t *record) { assert(!record->native_wait_set); }
void lpr_linux_fault_runtime_init(void) {}
void lpr_linux_register_thread_signal_hint(lpr_signal_thread_state_t *state) { (void)state; }
void lpr_linux_process_state_init(void) { lpr_linux_current_pid=41; }
void lpr_unix_context_destroy(struct lpr_unix_context *c) { (void)c;abort(); }
void lpr_linux_prepare_process_exit(uint64_t c) { (void)c;abort(); }
void lpr_state_lock(volatile uint32_t *p) { (void)p; }
void lpr_state_unlock(volatile uint32_t *p) { (void)p; }
void *lpr_memset(void *p,int c,size_t n) { return memset(p,c,n); }
void *lpr_memcpy(void *p,const void *s,size_t n) { return memcpy(p,s,n); }
int64_t lpr_pacha_syscall0(uint64_t n) { assert(n==PACHAOS_SYSCALL_GETTID);return 1; }
int64_t lpr_pacha_syscall1(uint64_t n,uint64_t a) { (void)a;return n==PACHAOS_SYSCALL_THREAD_START && fail_launch_stage==3 ? PACHA_SYSCALL_ERR_INVALID : 0; }
int64_t lpr_pacha_syscall6(uint64_t n,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
{
 (void)a;(void)b;(void)d;(void)e;(void)f;
 if(n==PACHAOS_SYSCALL_MMAP) {
  if(fail_launch_stage==1)return PACHA_SYSCALL_ERR_INVALID;
  void *allocation=calloc(1,c);assert(allocation);++live_launches;return (uintptr_t)allocation;
 }
 assert(n==PACHAOS_SYSCALL_THREAD_CREATE);
 if(fail_launch_stage==2)return PACHA_SYSCALL_ERR_INVALID;
 (*(lpr_thread_record_t **)(uintptr_t)c)->tid=2;return 42;
}
lpr_signal_thread_state_t *lpr_signal_thread_state_current(void) { static lpr_signal_thread_state_t state;return &state; }
int lpr_signal_thread_reserve_start(void) { return 0; }
void lpr_signal_thread_release_start_reservation(void) { ++released_reservations; }
void lpr_signal_thread_state_release_current(void) {}
void lpr_signal_thread_state_single_invalidate(void) {}
int64_t lpr_linux_sync_native_signal_mask(void) { return 0; }
void lpr_clone_thread_entry(void) { abort(); }
#endif
int main(void)
{
 lpr_state.thread_count=2;
#if INTERNAL_WAIT_MODE == 1
 lpr_block_lock_word=1;block_lock();block_unlock();assert(waits==1);
#elif INTERNAL_WAIT_MODE == 2
 volatile uint32_t word=1;lpr_state_lock(&word);lpr_state_unlock(&word);assert(waits==1);
 lpr_fd_table_t table={0};lpr_fd_table_configure_lock(&table,&lpr_state.thread_count,lpr_fd_lock_futex_wait,lpr_fd_lock_futex_wake);
 table.lock.word=1;lpr_fd_table_lock(&table);lpr_fd_table_unlock(&table);assert(waits==2);
#elif INTERNAL_WAIT_MODE == 3
 lpr_backend_slab_lock=1;lpr_backend_slab_acquire();lpr_backend_slab_release();assert(waits==1);
#elif INTERNAL_WAIT_MODE == 4
 lpr_netd_page_pool_wait(1);assert(waits==1);
#elif INTERNAL_WAIT_MODE == 5
 struct pacha_ipc_msg msg={0};assert(!lpr_native_ipc_recv_wait(42,&msg));assert(waits==1);
 assert(!lpr_native_ipc_send_wait(42,&msg));assert(waits==2 && sends==2);
 int sent = 0;
 sends=0;assert(!lpr_native_ipc_call_wait(42,&msg,&msg,&sent));assert(sent && waits==3 && sends==2);
 force_committed=1;sends=0;
 assert(lpr_native_ipc_call_wait(42,&msg,&msg,&sent)==PACHA_SYSCALL_ERR_NOT_READY);
 assert(sent && waits==3 && sends==1); /* Never replay a committed request. */
#elif INTERNAL_WAIT_MODE == 7
 assert(lpr_linux_thread_exists(0) && lpr_linux_thread_exists(1) && lpr_linux_thread_exists(41));
 assert(!lpr_linux_thread_exists(42) && !lpr_linux_thread_exists(UINT64_MAX));
 _Alignas(16) unsigned char stack[8192];uintptr_t top=(uintptr_t)&stack[8176];*(uint64_t *)top=123;
 /* No assumed libc function register; preserve arbitrary Linux state. */
 struct lpr_linux_user_frame frame={.r9=0,.r12=0x1234,.rip=0x5678};
 uint64_t flags=LPR_CLONE_VM|LPR_CLONE_FS|LPR_CLONE_FILES|LPR_CLONE_SIGHAND|LPR_CLONE_THREAD|LPR_CLONE_SETTLS;
 assert(lpr_linux_clone_thread(&frame,flags,top,0,0,0x1000)==2);assert(waits==1);
 lpr_thread_record_t *record=lpr_state.threads.head;
 assert(record->return_frame->rsp==top && *(uint64_t *)top==123);
 assert(record->return_frame->r9==0 && record->return_frame->r12==0x1234 && record->return_frame->rip==0x5678);
 record->parent_ready=0;
 assert(lpr_clone_thread_bootstrap(record)==record->return_frame);
 assert(waits==2 && record->started && record->parent_ready);
 lpr_thread_record_remove(record);
 assert(!lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP,record->allocation,LPR_THREAD_LAUNCH_BYTES));
 for(fail_launch_stage=1;fail_launch_stage<=3;++fail_launch_stage) {
  assert(lpr_linux_clone_thread(&frame,flags,top,0,0,0x1000)==-LPR_LINUX_EINVAL);
  assert(!live_launches && released_reservations==(unsigned)fail_launch_stage);
  assert(lpr_state.threads.head==&lpr_state.threads.main_thread);
 }
#elif INTERNAL_WAIT_MODE == 6
 volatile uint32_t word=1;wait_epoch(&word,1);assert(waits==1);
 struct lpr_fs_session local={0};local.doorbell=42;local.wake_read=43;
 assert(!wait_endpoint(&local,PACHA_FD_EVENT_READABLE));assert(waits==2);
 lpr_state.thread_count=1;assert(!receive_session(&local));assert(waits==3);
 lpr_state.thread_count=2;assert(!receive_session(&local));assert(waits==5);
#endif
 printf("uninterruptible wait internal wait mode %d PASS (%u native waits)\n",INTERNAL_WAIT_MODE,waits);
}

int64_t lpr_ipc_message(uint64_t nr,uint64_t fd,uint64_t request,
 uint64_t reply,uint64_t timeout,uint64_t flags,int *sent)
{
    if (nr != PACHA_IPC_SYSCALL_CALL) return test_lpr_ipc_noncall(nr, fd, request, reply, timeout, flags, sent);

 assert(nr==PACHAOS_SYSCALL_IPC_CALL && fd==42 && request && reply);
 assert(timeout==UINT64_MAX && flags==PACHA_WAIT_UNINTERRUPTIBLE);
 *sent=force_committed || sends!=0;
 return sends++==0?PACHA_SYSCALL_ERR_NOT_READY:0;
}
