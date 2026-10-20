/* Exercise real deadline/remaining-zero branches, preserving successful results. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../userland/personality/linux/runtime/lpr_wait.c"
#include "../userland/personality/linux/runtime/lpr_vfs/ops.c"
#define lpr_linux_dispatch_pending_signals_with_result unused_dispatch_pending_signals_with_result
#include "../userland/personality/linux/runtime/lpr_signal.c"
#undef lpr_linux_dispatch_pending_signals_with_result
static struct lpr_linux_user_frame user_frame;
const struct lpr_linux_user_frame *lpr_current_linux_user_frame(void) { return &user_frame; }

int64_t lpr_pacha_status_to_errno(int64_t s) { return pacha_kernel_status_to_errno(s); }
static uint64_t now_ns;
static int64_t native_result;
static unsigned waits, probes, kernel_pending, deliveries, event_drains, socket_marks;
static uint64_t returned_events;
lpr_state_t lpr_state;
void *lpr_memset(void *p, int c, size_t n) { return memset(p,c,n); }
int64_t lpr_pacha_syscall4(uint64_t n,uint64_t a,uint64_t b,uint64_t c,uint64_t flags)
{ if(b) ((struct pacha_pollfd *)(uintptr_t)a)[0].revents=returned_events; assert(n==PACHA_FD_SYSCALL_WAIT_MANY && flags==0); ++waits; if(c!=PACHA_FD_WAIT_FOREVER)now_ns+=c*1000000; return native_result; }
int64_t lpr_pacha_syscall3(uint64_t n,uint64_t a,uint64_t b,uint64_t c)
{ (void)n;(void)a;(void)b;(void)c; return -1; }
int64_t lpr_pacha_syscall2(uint64_t n,uint64_t a,uint64_t b)
{ if(n==PACHAOS_SYSCALL_PROCESS_SIGNAL_CTL){ assert(a==PACHAOS_PROCESS_SIGNAL_CTL_DELIVER_PENDING_FRAME && user_frame.rax==0); ++probes; if(kernel_pending){kernel_pending=0;++deliveries;} return 0; } assert(n==PACHAOS_SYSCALL_CLOCK_GETTIME); (void)a; struct pachaos_timespec *t=(void *)(uintptr_t)b; t->tv_sec=now_ns/1000000000; t->tv_nsec=now_ns%1000000000; return 0; }
int64_t lpr_pacha_syscall1(uint64_t n,uint64_t a)
{ assert(n==PACHAOS_SYSCALL_NANOSLEEP); const struct pachaos_timespec *t=(void *)(uintptr_t)a; now_ns+=t->tv_sec*1000000000+t->tv_nsec; ++waits; return native_result; }
int64_t lpr_close_native_fd_if_open(uint64_t fd) { (void)fd;return 0; }
int lpr_linux_socket_fd_active(uint64_t fd) { return fd==42; }
void lpr_linux_socket_mark_readable(uint64_t fd) { assert(fd==42); ++socket_marks; }
void lpr_eventfd_drain_wait(uint64_t fd) { assert(fd==42); ++event_drains; }
int64_t lpr_unix_poll_block(lpr_wait_graph_t *g,const lpr_wait_deadline_t *d,int64_t (*block)(lpr_wait_graph_t *,const lpr_wait_deadline_t *))
{ return block(g,d); }
int64_t lpr_linux_dispatch_pending_signals_with_result(int64_t result) { (void)result;return 0; }
int main(void)
{
 for(unsigned signal=0;signal<2;++signal)
 for(unsigned interrupted=0;interrupted<2;++interrupted){
  lpr_native_pending_hint_active=1; lpr_native_pending_hint=0; lpr_state.thread_count=1;
  native_result=interrupted?PACHA_SYSCALL_ERR_NOT_READY:0;
  for(unsigned leaves=0;leaves<2;++leaves){
   now_ns=waits=probes=deliveries=0; kernel_pending=signal; unsigned observed=0;
   lpr_wait_graph_t g; lpr_wait_graph_init(&g);
   g.leaf_count=leaves; g.leaves[0].fd=42; g.relative_deadline_ns=1000000;
   assert(lpr_wait_graph_block(&g,0,&observed)==0);
   assert(waits==1 && observed==interrupted);
   /* Pre-fix ordinary result with hint zero skips the pending query. */
   lpr_linux_deliver_native_pending_frame(0); assert(probes==0 && kernel_pending==signal);
   lpr_linux_deliver_native_pending_frame_after_wait(0,&observed);
   assert(probes==interrupted && observed==0);
   assert(deliveries==(signal && interrupted) && kernel_pending==(signal && !interrupted));
  }
  now_ns=waits=probes=deliveries=0; kernel_pending=signal; unsigned observed=0;
  struct pachaos_timespec req={0,1000000},rem={99,99};
  assert(lpr_linux_nanosleep((uintptr_t)&req,(uintptr_t)&rem,&observed)==0);
  assert(waits==1 && observed==interrupted);
  if(interrupted)assert(rem.tv_sec==0 && rem.tv_nsec==0);
  /* Pre-fix ordinary result with hint zero skips the pending query. */
   lpr_linux_deliver_native_pending_frame(0); assert(probes==0 && kernel_pending==signal);
   lpr_linux_deliver_native_pending_frame_after_wait(0,&observed);
  assert(probes==interrupted);
  assert(deliveries==(signal && interrupted) && kernel_pending==(signal && !interrupted));
 }
 /* Completion uses output events, never the independent byte minimum. */
 for(unsigned native=0;native<2;++native)
 for(unsigned ready=0;ready<2;++ready){
  lpr_wait_graph_t g; lpr_wait_graph_init(&g);
  g.leaf_count=1; g.leaves[0].fd=42;
  g.leaves[0].events=PACHA_FD_EVENT_READABLE;
  g.leaves[0].min_write_bytes=ready?0:PACHA_FD_EVENT_READABLE;
  g.logical_fds[0]=42;
  g.drain_modes[0]=native?LPR_WAIT_DRAIN_NATIVE:LPR_WAIT_DRAIN_EVENT;
  g.relative_deadline_ns=1000000;
  returned_events=ready?PACHA_FD_EVENT_READABLE:0; native_result=ready;
  event_drains=socket_marks=0;
  unsigned observed=0;
  assert(lpr_wait_graph_block(&g,0,&observed)==0);
  assert(event_drains==(!native && ready));
  assert(socket_marks==(native && ready));
 }
 puts("uninterruptible wait wait graph deadline and relative nanosleep zero preserve result/local NOT_READY PASS");
}

#include "lpr_ipc_mock.h"
int64_t lpr_ipc_message(uint64_t nr, uint64_t fd, uint64_t send, uint64_t receive,
    uint64_t timeout, uint64_t flags, int *committed) {
    if (nr==PACHAOS_SYSCALL_IPC_RECV) { assert(fd==42); return PACHA_SYSCALL_ERR_NOT_READY; }
    return test_lpr_ipc_noncall(nr, fd, send, receive, timeout, flags, committed);
}
