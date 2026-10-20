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
#endif
lpr_state_t lpr_state;
static unsigned waits, sends;
static int force_committed;
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
{ (void)a;(void)b; assert(n!=PACHAOS_SYSCALL_IPC_CALL); if(n==PACHAOS_SYSCALL_IPC_SEND) return sends++==0?PACHA_SYSCALL_ERR_NOT_READY:0;return 0; }
#if INTERNAL_WAIT_MODE != 2
int64_t lpr_pacha_status_to_errno(int64_t s) { return pacha_kernel_status_to_errno(s); }
#endif
#if INTERNAL_WAIT_MODE == 7
void lpr_unix_context_destroy(struct lpr_unix_context *c) { (void)c;abort(); }
void lpr_linux_prepare_process_exit(uint64_t c) { (void)c;abort(); }
static jmp_buf child_started;
static int start_child(void *arg) { assert(arg==(void *)123);longjmp(child_started,1); }
void lpr_state_lock(volatile uint32_t *p) { (void)p; }
void lpr_state_unlock(volatile uint32_t *p) { (void)p; }
void *lpr_memset(void *p,int c,size_t n) { return memset(p,c,n); }
int64_t lpr_pacha_syscall0(uint64_t n) { assert(n==PACHAOS_SYSCALL_GETTID);return 1; }
int64_t lpr_pacha_syscall1(uint64_t n,uint64_t a) { (void)n;(void)a;return 0; }
int64_t lpr_pacha_syscall6(uint64_t n,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
{ (void)a;(void)b;(void)d;(void)e;(void)f;assert(n==PACHAOS_SYSCALL_THREAD_CREATE);((lpr_thread_record_t *)(uintptr_t)c)->tid=2;return 42; }
lpr_signal_thread_state_t *lpr_signal_thread_state_current(void) { static lpr_signal_thread_state_t state;return &state; }
int lpr_signal_thread_reserve_start(void) { return 0; }
void lpr_signal_thread_release_start_reservation(void) {}
void lpr_signal_thread_state_release_current(void) {}
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
 _Alignas(16) unsigned char stack[8192];uintptr_t top=(uintptr_t)&stack[8176];*(uint64_t *)top=123;
 struct lpr_linux_user_frame frame={.r9=(uintptr_t)start_child};
 uint64_t flags=LPR_CLONE_VM|LPR_CLONE_FS|LPR_CLONE_FILES|LPR_CLONE_SIGHAND|LPR_CLONE_THREAD|LPR_CLONE_SETTLS;
 assert(lpr_linux_clone_thread(&frame,flags,top,0,0,0x1000)==2);assert(waits==1);
 lpr_thread_record_t record={.start_function=(uintptr_t)start_child,.start_argument=123};
 if(!setjmp(child_started))lpr_clone_thread_bootstrap(&record);assert(waits==2 && record.started && record.parent_ready);
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
