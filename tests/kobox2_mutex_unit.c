/* SPDX-License-Identifier: MIT */
/* Exercise the adapter's real mutex with Linux futexes standing in for native
 * syscalls, including unlock between waiter publication and kernel entry. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <pacha/abi.h>

void ph_lock(atomic_uint *lock);
void ph_unlock(atomic_uint *lock);
static atomic_uint lock_word, waits, wakes, delay_wait, entered_wait;
static unsigned protected_count;

long pacha_syscall1(uint64_t nr, uint64_t a0) {
    (void)nr; (void)a0;
    abort();
}

long pacha_syscall2(uint64_t nr, uint64_t a0, uint64_t a1) {
    if (nr == 1) {
        return write(STDERR_FILENO, (const void *)(uintptr_t)a0, a1);
    }
    assert(nr == PACHA_RUNTIME_SYSCALL_FUTEX_WAKE);
    atomic_fetch_add(&wakes, 1);
    return syscall(SYS_futex, (void *)(uintptr_t)a0, FUTEX_WAKE_PRIVATE,
                   a1 > INT_MAX ? INT_MAX : (int)a1, NULL, NULL, 0);
}

long pacha_syscall3(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2) {
    assert(nr == PACHA_RUNTIME_SYSCALL_FUTEX_WAIT && a2 == 0);
    atomic_fetch_add(&waits, 1);
    atomic_store(&entered_wait, 1);
    while (atomic_load(&delay_wait)) sched_yield();
    long rc = syscall(SYS_futex, (void *)(uintptr_t)a0, FUTEX_WAIT_PRIVATE,
                      (unsigned)a1, NULL, NULL, 0);
    if (rc == -1 && errno == EAGAIN) return PACHA_SYSCALL_ERR_NOT_READY;
    assert(rc == 0);
    return rc;
}

static void *increment(void *iterations) {
    for (uintptr_t i = 0; i < (uintptr_t)iterations; ++i) {
        ph_lock(&lock_word);
        unsigned previous = protected_count;
        if ((i & 127) == 0) sched_yield();
        protected_count = previous + 1;
        ph_unlock(&lock_word);
    }
    return NULL;
}

int main(void) {
    /* An uncontended lock must not enter the kernel. */
    increment((void *)(uintptr_t)10000);
    assert(protected_count == 10000);
    assert(atomic_load(&wakes) == 0 && atomic_load(&waits) == 0);

    /* The owner releases before the wait syscall can check its expected value.
     * The waiter must retry, not sleep through that wake. */
    pthread_t thread;
    atomic_store(&delay_wait, 1);
    ph_lock(&lock_word);
    assert(pthread_create(&thread, NULL, increment, (void *)(uintptr_t)1) == 0);
    while (!atomic_load(&entered_wait)) sched_yield();
    ph_unlock(&lock_word);
    atomic_store(&delay_wait, 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(protected_count == 10001 && atomic_load(&wakes) != 0);

    /* Multiple sleepers must keep receiving wakeups even when a new fast-path
     * owner acquires the mutex before an already-woken waiter. */
    pthread_t workers[6];
    for (unsigned i = 0; i < 6; ++i)
        assert(pthread_create(&workers[i], NULL, increment, (void *)(uintptr_t)40000) == 0);
    for (unsigned i = 0; i < 6; ++i) assert(pthread_join(workers[i], NULL) == 0);
    assert(protected_count == 250001 && atomic_load(&lock_word) == 0);
    puts("kobox2 adapter mutex: PASS");
}
