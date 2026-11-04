/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../userland/kobox2_adapter/fs_queue.c"
static struct pacha_ipc_msg incoming, outgoing;
static int receive_status=PACHA_SYSCALL_ERR_EMPTY, send_status, collect_status=-EAGAIN;
static unsigned submitted, notifications;
static uint64_t current_ns=2000000000;
_Noreturn void ph_fail(const char *file, unsigned line, uint64_t result) {
 fprintf(stderr,"%s:%u %llu\n",file,line,(unsigned long long)result); abort();
}
int ph_ipc_send(struct ph_ipc *ipc, struct ph_ipc_packet *packet) {
 (void)ipc; (void)packet; ++notifications; return 0;
}
long pacha_syscall2(uint64_t nr,uint64_t clock,uint64_t ptr) {
 assert(nr==PACHA_RUNTIME_SYSCALL_CLOCK_GETTIME && clock==1);
 uint64_t *v=(void *)(uintptr_t)ptr;
 v[0]=current_ns/1000000000; v[1]=current_ns%1000000000; return 0;
}
long pacha_ipc_registers(uint64_t nr,uint64_t fd,uint64_t flags,uint64_t timeout,
 uint64_t words[4],struct pacha_ipc_extension *extension,int *committed) {
 assert(fd==60 && !flags && !timeout && !extension && !committed);
 if(nr==PACHA_IPC_SYSCALL_SEND) {
  outgoing=(struct pacha_ipc_msg){.word0=words[0],.word1=words[1],.word2=words[2],.word3=words[3]};
  return send_status;
 }
 assert(nr==PACHA_IPC_SYSCALL_RECV);
 if(!receive_status) {
  words[0]=incoming.word0; words[1]=incoming.word1;
  words[2]=incoming.word2; words[3]=incoming.word3;
  receive_status=PACHA_SYSCALL_ERR_EMPTY; return 0;
 }
 return receive_status;
}
static int submit(uint64_t pages,uint64_t budget) {
 assert(pages==1024 && budget==100000000); ++submitted; return 0;
}
static int collect(struct kobox_linux_pressure_result *result) {
 if(!collect_status) *result=(struct kobox_linux_pressure_result){1024,32,512};
 return collect_status;
}
static void init(struct ph_fs_queue *q) {
 memset(q,0,sizeof(*q)); atomic_flag_clear(&q->watch_lock);
 q->pressure=(struct ph_ipc){.fd=60,.generation=1,.admitted=1};
 q->wake_tx.admitted=1; q->pressure_submit=submit; q->pressure_collect=collect;
}
static void request(uint64_t seq) {
 incoming=(struct pacha_ipc_msg){.word0=pacha_pressure_tag(seq,PACHA_PRESSURE_REQUEST),
  .word1=1024,.word2=current_ns+1000000000}; receive_status=0;
}
int main(void) {
 struct ph_fs_queue q; init(&q);
 assert(!pressure_advance(&q) && !q.pressure_dead); /* empty native recv is +5 */
 receive_status=PACHA_SYSCALL_ERR_NOT_READY;
 assert(!pressure_advance(&q) && !q.pressure_dead);
 request(1); assert(pressure_advance(&q)==PH_LIFECYCLE_SERVICE_PROGRESS && submitted==1);
 assert(q.pressure_pending && !q.pressure_have_result);
 assert(!pressure_advance(&q) && submitted==1); /* never waits for Linux work */
 collect_status=0; send_status=PACHA_SYSCALL_ERR_NOT_READY;
 assert(!pressure_advance(&q) && q.pressure_have_result && q.pressure_pending);
 assert(outgoing.word0==pacha_pressure_tag(1,PACHA_PRESSURE_ACK) && outgoing.word1==1024 &&
  outgoing.word2==32 && outgoing.word3==512);
 send_status=0; assert(pressure_advance(&q)==PH_LIFECYCLE_SERVICE_PROGRESS);
 assert(!q.pressure_pending && !q.pressure_have_result);
 request(1); pressure_advance(&q); assert(q.pressure_dead && notifications==1); /* replay */
 init(&q); request(2); incoming.word1=1025;
 pressure_advance(&q); assert(q.pressure_dead && submitted==1);
 init(&q); request(3); incoming.word2=current_ns;
 pressure_advance(&q); assert(q.pressure_dead && submitted==1);
 init(&q); receive_status=PACHA_SYSCALL_ERR_CLOSED;
 pressure_advance(&q); assert(q.pressure_dead && submitted==1);
 puts("kobox2 pressure: async submit ack backpressure replay deadline owner-close PASS");
}
