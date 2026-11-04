/* SPDX-License-Identifier: MIT */
#include "ready.h"
#include <pacha/syscall.h>
#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
enum { STACK_BYTES = 65536 };
struct native_worker {
    pacha_fd_t fd;
    void *stack;
};
extern void gpud_ready_native_entry(void);
int gpud_ready_os_event(pacha_fd_t *fd) {
    *fd = pacha_eventfd_create(0,
                               PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_READ | PACHA_FD_RIGHT_WRITE |
                                   PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_WAIT,
                               PACHA_FD_FLAG_CLOEXEC);
    return *fd >= 16 ? 0 : -ENOMEM;
}
int gpud_ready_os_signal(pacha_fd_t fd) {
    uint64_t one = 1;
    long r = pacha_fd_write(fd, &one, 8);
    return r == 8 || r == PACHA_ERR_NOT_READY || r == PACHA_SYSCALL_ERR_NOT_READY ? 0 : -EIO;
}
int gpud_ready_os_drain(pacha_fd_t fd) {
    uint64_t value;
    long r = pacha_fd_read(fd, &value, 8);
    return r == 8 || r == PACHA_ERR_NOT_READY || r == PACHA_SYSCALL_ERR_NOT_READY ||
                   r == PACHA_SYSCALL_ERR_EMPTY || r == -PACHA_SYSCALL_ERR_EMPTY
               ? 0
               : -EIO;
}
int gpud_ready_os_wait(struct pacha_fd_wait_retained *set, struct pacha_pollfd *p, size_t n,
                       uint64_t timeout) {
    long r = pacha_fd_wait_many_retained(set, p, n, timeout);
    int ready = 0;
    for (size_t i = 0; i < n; i++)
        ready += p[i].revents != 0;
    if (ready)
        return ready;
    if (r == PACHA_ERR_NOT_READY || r == PACHA_SYSCALL_ERR_NOT_READY || !r)
        return 0;
    return -EIO;
}
int gpud_ready_os_ack(void *context, pacha_fd_t ack) {
    struct native_worker *w = context;
    struct pacha_pollfd p[2] = {
        {.fd = ack, .events = PACHA_FD_EVENT_READABLE},
        {.fd = w->fd, .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP}};
    long r = pacha_fd_wait_many(p, 2, UINT64_MAX);
    if (r > 0 && p[0].revents)
        return 0;
    return -EIO;
}
int gpud_ready_os_start(struct gpud_ready_worker *w, void (*entry)(struct gpud_ready_worker *),
                        void **out) {
    if (entry != gpud_ready_worker_run)
        return -EINVAL;
    struct native_worker *n = calloc(1, sizeof(*n));
    if (!n)
        return -ENOMEM;
    n->stack =
        pacha_mmap_anonymous(STACK_BYTES, PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_PRIVATE);
    if (!n->stack) {
        free(n);
        return -ENOMEM;
    }
    /* Populate the control stack before publication. Worker has no musl TLS and
     * never calls malloc/stdio or GPU code; syscall and atomics only. */
    for (unsigned i = 0; i < STACK_BYTES; i += 4096)
        ((volatile unsigned char *)n->stack)[i] = 0;
    uintptr_t *sp = (void *)((uintptr_t)n->stack + STACK_BYTES - 8);
    *sp = (uintptr_t)w;
    n->fd = pacha_thread_create(PACHA_PROCESS_SELF_FD, (uintptr_t)gpud_ready_native_entry,
                                (uintptr_t)sp, 0, 0,
                                PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL |
                                    PACHA_FD_RIGHT_START | PACHA_FD_RIGHT_INSPECT);
    if (n->fd < 16) {
        (void)pacha_munmap(n->stack, STACK_BYTES);
        free(n);
        return -ENOMEM;
    }
    if (pacha_thread_start(n->fd)) {
        /* A created but unstarted thread cannot use the stack. */
        (void)pacha_fd_close(n->fd);
        (void)pacha_munmap(n->stack, STACK_BYTES);
        free(n);
        return -EIO;
    }
    *out = n;
    return 0;
}
int gpud_ready_os_join(void *context) {
    struct native_worker *n = context;
    if (!n)
        return 0;
    struct pacha_pollfd p = {.fd = n->fd,
                             .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP};
    long r = pacha_fd_wait_many(&p, 1, UINT64_MAX);
    if (r <= 0 || !p.revents)
        return -EIO;
    if (pacha_fd_close(n->fd) || pacha_munmap(n->stack, STACK_BYTES))
        return -EIO;
    free(n);
    return 0;
}
int gpud_ready_os_close(pacha_fd_t fd) { return pacha_fd_close(fd); }
