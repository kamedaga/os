/* SPDX-License-Identifier: MIT */
/* Native threads started directly on NEW fibers (abort and secondary idle):
 * the joiner releases the stack while the thread may still run on it. Native
 * thread syscalls are modelled with pthreads; each runs on the fiber stack. */
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#define PH_TLS_TEST_HOOKS 1
#include "../userland/kobox2_adapter/host.h"

static _Thread_local struct ph_task *tls_task;
struct ph_task *ph_test_current_task(void) { return tls_task; }
#define ph_current_task ph_test_current_task
#include "../userland/kobox2_adapter/native.c"

enum { THREADS = 64 };
struct native_thread {
    pthread_t thread;
    uintptr_t entry, stack;
    struct ph_task *task;
    jmp_buf exit;
    bool started;
};
static struct native_thread natives[THREADS];
static unsigned created;
static atomic_uint entered, released;

_Noreturn void ph_fail(const char *file, unsigned line, uint64_t result) {
    fprintf(stderr, "%s:%u result=%llu\n", file, line, (unsigned long long)result);
    abort();
}
void ph_exception(struct pacha_native_fault_frame *frame) { (void)frame; abort(); }
void ph_dispatch_notifications(void) { abort(); }
void *ph_alloc(size_t size) {
    void *memory = aligned_alloc(PH_PAGE_SIZE, (size + PH_PAGE_SIZE - 1) & ~(PH_PAGE_SIZE - 1));
    assert(memory);
    memset(memory, 0, size);
    return memory;
}
void ph_free(void *base, size_t size) {
    /* Poison: a thread still running on a released stack would fault. */
    memset(base, 0xa5, size);
    free(base);
}

static struct native_thread *native(uint64_t fd) {
    assert(fd >= 16 && fd - 16 < created);
    return &natives[fd - 16];
}

static void *native_main(void *opaque) {
    struct native_thread *self = opaque;

    tls_task = self->task;
    if (!setjmp(self->exit)) {
        /* THREAD_CREATE's entry and stack: jump onto the fiber's frame. */
        __asm__ volatile("mov %0, %%rsp; jmp *%1" :: "r"(self->stack), "r"(self->entry) : "memory");
        __builtin_unreachable();
    }
    return NULL;
}

long pacha_syscall1(uint64_t nr, uint64_t a) {
    if (nr == PACHA_THREAD_SYSCALL_START) {
        struct native_thread *self = native(a);
        assert(!self->started);
        self->started = true;
        assert(!pthread_create(&self->thread, NULL, native_main, self));
        return 0;
    }
    assert(nr == PACHA_FD_SYSCALL_CLOSE);
    return 0;
}
long pacha_syscall2(uint64_t nr, uint64_t a, uint64_t b) {
    if (nr == PACHA_THREAD_SYSCALL_WAIT) {
        uint64_t *status = (uint64_t *)(uintptr_t)b;
        (void)a;
        status[0] = 2;
        status[1] = 0;
        return 0;
    }
    assert(nr == PACHA_PROCESS_SYSCALL_SIGNAL_CTL && a == PACHA_PROCESS_SIGNAL_CTL_SET_MASK);
    return 0;
}
long pacha_syscall3(uint64_t nr, uint64_t a, uint64_t b, uint64_t c) {
    if (nr == PACHA_THREAD_SYSCALL_EXIT) {
        /* Leave the fiber stack for the pthread's own, as a native exit
         * leaves the stack untouched for the joiner. */
        for (unsigned i = 0; i < created; ++i)
            if (natives[i].task == tls_task)
                longjmp(natives[i].exit, 1);
        abort();
    }
    assert(nr == PACHA_VM_SYSCALL_MPROTECT && a && b == PH_PAGE_SIZE && !c);
    return 0;
}
long pacha_syscall4(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    if (nr == PACHA_FD_SYSCALL_WAIT_MANY) {
        struct pacha_pollfd *fds = (struct pacha_pollfd *)(uintptr_t)a;
        assert(b == 1 && c == PACHA_FD_WAIT_FOREVER && !d);
        assert(!pthread_join(native(fds[0].fd)->thread, NULL));
        fds[0].revents = PACHA_FD_EVENT_READABLE;
        return 1;
    }
    assert(nr == PACHA_PROCESS_SYSCALL_SIGNAL_CTL);
    return 0;
}
long pacha_syscall6(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d,
    uint64_t e, uint64_t f) {
    if (nr == PACHA_THREAD_SYSCALL_CREATE) {
        assert(a == PACHA_PROCESS_SELF_FD && !d && f && created < THREADS);
        struct native_thread *self = &natives[created];
        self->entry = b;
        self->stack = c;
        self->task = (struct ph_task *)(uintptr_t)e;
        return 16 + created++;
    }
    assert(nr == PACHA_PROCESS_SYSCALL_SIGNAL_CTL);
    return 0;
}

/* port.c's task_bootstrap after an abort: it returns at once. */
static void *aborted_entry(void *argument) {
    assert(argument == &entered && ph_current_fiber()->origin == ph_current_task());
    atomic_fetch_add(&entered, 1);
    return NULL;
}

/* The secondary idle task leaving its CPU: still running when joined. */
static void *idle_entry(void *argument) {
    (void)argument;
    atomic_fetch_add(&entered, 1);
    while (!atomic_load(&released))
        sched_yield();
    struct timespec pause = {.tv_nsec = 1000000};
    nanosleep(&pause, NULL);
    return NULL;
}

int main(void) {
    static struct ph_task main_thread;
    main_thread.self = &main_thread;
    tls_task = &main_thread;
    for (unsigned i = 0; i < 32; ++i) {
        struct ph_fiber *fiber = ph_fiber_create(aborted_entry, &entered);
        assert(ph_fiber_thread_start(fiber)->fiber == fiber);
        /* exit_thread: wake, then join_destroy immediately. */
        ph_fiber_destroy(fiber);
    }
    assert(atomic_load(&entered) == 32);
    for (unsigned i = 0; i < 8; ++i) {
        struct ph_fiber *fiber = ph_fiber_create(idle_entry, NULL);
        (void)ph_fiber_thread_start(fiber);
        while (atomic_load(&entered) != 33 + i)
            sched_yield();
        /* smp shutdown: the thread is on the fiber stack when joined. */
        atomic_store(&released, 1);
        ph_fiber_destroy(fiber);
        atomic_store(&released, 0);
    }
    assert(created == 40);
    puts("kobox2 fiber origin: PASS abort wake+join, joined while running on the fiber stack");
    return 0;
}
