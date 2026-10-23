/* SPDX-License-Identifier: MIT */
#include "filed/native_thread.h"
#include <pacha/ipc.h>
#include <pacha/syscall.h>
#include <pthread_impl.h>
#include <libc.h>
#include <stdint.h>
#include <string.h>

extern void *__copy_tls(unsigned char *);
extern int __init_tp(void *);

struct filed_thread_start {
    void (*entry)(void *);
    void *context;
    struct pthread *tls;
};
static struct filed_thread_start pager_thread_start;

static _Noreturn void filed_thread_entry(void)
{
    struct filed_thread_start start=pager_thread_start;
    if (__init_tp(start.tls)==0) start.entry(start.context);
    (void)pacha_syscall1(PACHA_THREAD_SYSCALL_EXIT,0);
    __builtin_unreachable();
}

int filed_native_thread_start(void (*entry)(void *),void *context,
    pacha_fd_t *thread_fd)
{
    if (!entry || !thread_fd || !libc.tls_size) return -1;
    enum { stack_size = 256 * 1024 };
    void *stack=pacha_mmap_anonymous(stack_size,PACHA_PROT_READ|PACHA_PROT_WRITE,
        PACHA_MMAP_PRIVATE);
    void *tls=pacha_mmap_anonymous((libc.tls_size+4095)&~(size_t)4095,
        PACHA_PROT_READ|PACHA_PROT_WRITE,PACHA_MMAP_PRIVATE);
    if (!stack || !tls) return -1;
    /* The pager must never fault on its own control stack or TLS. These are
     * anonymous mappings; touching every page makes them resident before the
     * worker can receive a file fault. */
    for (size_t i=0;i<stack_size;i+=4096)
        ((volatile unsigned char *)stack)[i]=0;
    for (size_t i=0;i<((libc.tls_size+4095)&~(size_t)4095);i+=4096)
        ((volatile unsigned char *)tls)[i]=0;
    struct pthread *td=__copy_tls(tls);
    td->self=td;
    td->detach_state=DT_DETACHED;
    td->locale=&libc.global_locale;
    td->next=td->prev=td;
    td->robust_list.head=&td->robust_list.head;
    pager_thread_start=(struct filed_thread_start){entry,context,td};
    uintptr_t *rsp=(uintptr_t *)((uintptr_t)stack+stack_size-8);
    *rsp=0;
    libc.threaded=1;
    libc.need_locks=1;
    const uint64_t rights=PACHA_FD_RIGHT_INSPECT|PACHA_FD_RIGHT_CLOSE|
        PACHA_FD_RIGHT_START|PACHA_FD_RIGHT_WAIT;
    pacha_fd_t fd=pacha_thread_create(PACHA_PROCESS_SELF_FD,
        (uintptr_t)filed_thread_entry,(uintptr_t)rsp,0,
        (uintptr_t)TP_ADJ(td),rights);
    if (fd<16) return -1;
    if (pacha_thread_start(fd)) { (void)pacha_fd_close(fd);return -1; }
    *thread_fd=fd;
    return 0;
}
