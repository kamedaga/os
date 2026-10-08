/* SPDX-License-Identifier: MIT */
/* Native mask implementation under real asynchronous POSIX signal delivery. */
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
/* No native FS here: thread words go through test_current_task(). */
#define PH_TLS_TEST_HOOKS 1
#include "../userland/kobox2_adapter/host.h"

static struct ph_task task;
static struct ph_task *test_current_task(void) { return &task; }
struct ph_task *ph_test_current_task(void) { return test_current_task(); }
#define ph_current_task test_current_task
#include "../userland/kobox2_adapter/native.c"

static atomic_uint sender_done, handled, callbacks;
static volatile sig_atomic_t critical;
static pthread_t recipient;

_Noreturn void ph_fail(const char *file, unsigned line, uint64_t result) {
    fprintf(stderr, "%s:%u result=%llu\n", file, line, (unsigned long long)result);
    abort();
}

void ph_dispatch_notifications(void) {
    uint64_t mask;
    assert(!critical);
    assert(!ph_notifications_save(&mask) && !mask);
    critical = 1;
    atomic_fetch_add_explicit(&callbacks, 1, memory_order_relaxed);
    critical = 0;
    assert(!ph_notifications_restore(mask));
}

static void notification(int signo) {
    assert(signo == SIGUSR1);
    atomic_fetch_add_explicit(&handled, 1, memory_order_relaxed);
    if (atomic_load_explicit(&task.notification_mask, memory_order_acquire)) {
        atomic_store_explicit(&task.notification_deferred, 1, memory_order_release);
        return;
    }
    /* An unmasked interruption may itself save/restore the same task's mask.
     * It must return with the interrupted state intact, including when it
     * arrived between the owner's load of the old mask and its store. */
    assert(!critical);
    assert(!ph_notifications_dispatch_preserved());
}

static void *send_signals(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < 10000; ++i) {
        assert(!pthread_kill(recipient, SIGUSR1));
        sched_yield();
    }
    atomic_store_explicit(&sender_done, 1, memory_order_release);
    return NULL;
}

int main(void) {
    struct sigaction action = { .sa_handler = notification, .sa_flags = SA_RESTART };
    sigemptyset(&action.sa_mask);
    assert(!sigaction(SIGUSR1, &action, NULL));
    recipient = pthread_self();
    pthread_t sender;
    assert(!pthread_create(&sender, NULL, send_signals, NULL));
    uint64_t iterations = 0;
    do {
        uint64_t outer, inner;
        assert(!ph_notifications_save(&outer) && !outer);
        critical = 1;
        assert(!ph_notifications_save(&inner) && inner == PH_NOTIFICATION_MASK);
        assert(!ph_notifications_restore(inner));
        assert(atomic_load_explicit(&task.notification_mask, memory_order_acquire) == PH_NOTIFICATION_MASK);
        critical = 0;
        assert(!ph_notifications_restore(outer));
        assert(!atomic_load_explicit(&task.notification_mask, memory_order_acquire));
        ++iterations;
    } while (!atomic_load_explicit(&sender_done, memory_order_acquire) || iterations < 100000);
    assert(!pthread_join(sender, NULL));
    assert(atomic_load_explicit(&handled, memory_order_relaxed));
    assert(atomic_load_explicit(&callbacks, memory_order_relaxed));
    assert(!atomic_load_explicit(&task.notification_deferred, memory_order_acquire));
    printf("native notification mask: PASS nested/async signal, iterations=%llu handled=%u callbacks=%u\n",
        (unsigned long long)iterations, atomic_load(&handled), atomic_load(&callbacks));
    return 0;
}
