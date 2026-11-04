/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../lifecycle.c"
static struct pacha_ipc_msg request, ack;
static int available, blocked, collected, submissions;
static uint64_t budget_seen;
long pacha_ipc_registers(uint64_t op, uint64_t fd, uint64_t flags,
    uint64_t timeout, uint64_t words[4], struct pacha_ipc_extension *ext, int *committed) {
    (void)flags;(void)timeout;(void)committed; assert(fd==99 && !ext);
    if (op==PACHA_IPC_SYSCALL_SEND) {
        if (blocked) return PACHA_SYSCALL_ERR_NOT_READY;
        ack=(struct pacha_ipc_msg){.word0=words[0],.word1=words[1],.word2=words[2],.word3=words[3]};
        return 0;
    }
    assert(op==PACHA_IPC_SYSCALL_RECV);
    if (!available) return PACHA_SYSCALL_ERR_EMPTY;
    available=0;
    words[0]=request.word0;words[1]=request.word1;words[2]=request.word2;words[3]=request.word3;
    return 0;
}
long pacha_syscall2(uint64_t op,uint64_t clock,uint64_t out) {
    assert(op==PACHA_RUNTIME_SYSCALL_CLOCK_GETTIME && clock==1);
    uint64_t *time=(void *)(uintptr_t)out; time[0]=1;time[1]=0;return 0;
}
static int submit(uint64_t pages,uint64_t budget) {
    assert(pages==32);budget_seen=budget;++submissions;return 0;
}
static int collect(struct kobox_linux_pressure_result *result) {
    if (!collected) return -EAGAIN;
    *result=(struct kobox_linux_pressure_result){32,8,24};return 0;
}
static int starts, stops, destroys;
void ph_wake(atomic_uint *word) { assert(atomic_load(word)==WORK_DONE); }
_Noreturn void ph_fail(const char *file,unsigned line,uint64_t result) {
    (void)file;(void)line;(void)result;abort();
}
int ph_ipc_destroy(struct ph_ipc *ipc,uint64_t generation) {
    (void)generation;ipc->admitted=0;++destroys;return 0;
}
static int start_pressure(int (*wake)(void *),void *context) {
    ++starts;assert(!wake(context));return 0;
}
static void stop_pressure(void) { ++stops; }
static int bad_dispatch(void *context,void *linux_service) {
    (void)context;(void)linux_service;return -EIO;
}
static int release_request(void *context) { (void)context;return 0; }
int main(void) {
    struct ph_lifecycle l={.pressure={.fd=99,.admitted=1},
        .pressure_submit=submit,.pressure_collect=collect};
    request=(struct pacha_ipc_msg){.word0=pacha_pressure_tag(1,PACHA_PRESSURE_REQUEST),
        .word1=32,.word2=2000000000};available=1;
    assert(!pressure_advance(&l) && l.pressure_pending && submissions==1);
    assert(budget_seen==100000000); /* duration, not native absolute time */
    assert(!pressure_advance(&l) && !ack.word0 && submissions==1);
    collected=blocked=1;
    assert(!pressure_advance(&l) && l.pressure_result_ready && l.pressure_pending);
    blocked=0;
    assert(!pressure_advance(&l) && !l.pressure_pending && !l.pressure_result_ready);
    assert(ack.word0==pacha_pressure_tag(1,PACHA_PRESSURE_ACK));
    assert(ack.word1==32 && ack.word2==8 && ack.word3==24);
    available=1;assert(pressure_advance(&l)==-EPROTO); /* replay */
    request.word0=pacha_pressure_tag(2,PACHA_PRESSURE_REQUEST);
    request.word2=1000000000;available=1;
    assert(pressure_advance(&l)==-ETIMEDOUT && submissions==1);
    request.word2=1100000000;request.word1=1025;available=1;
    assert(pressure_advance(&l)==-EPROTO && submissions==1);
    l.pressure_start=start_pressure;l.pressure_stop=stop_pressure;
    l.pressure_pending=l.pressure_result_ready=0;
    l.pressure_job=1;atomic_store(&l.work,WORK_READY);
    assert(!dispatch(&l,NULL) && starts==1 && l.pressure_started && !stops);
    l.pressure_job=2;atomic_store(&l.work,WORK_READY);
    assert(!dispatch(&l,NULL) && !l.pressure_started && stops==1);
    request.word0=0;available=1;l.pressure_job=1;atomic_store(&l.work,WORK_READY);
    assert(!dispatch(&l,NULL) && starts==2 && stops==2 && destroys==1 && !l.pressure.admitted);
    l.pressure_started=1;l.service.dispatch=bad_dispatch;l.service.release=release_request;
    atomic_store(&l.work,WORK_READY);
    assert(dispatch(&l,NULL)==-EIO && stops==3 && !l.pressure_started);
    l.monitored=true;l.pressure_started=1;
    assert(!abort_monitor(&l,-ETIMEDOUT) && stops==4 && !l.pressure_started);
    puts("GPU/Wi-Fi pressure endpoint: async result, backpressure, ACK, replay, deadline, bound, Linux dispatch, stop/error/monitor cleanup PASS");
}
