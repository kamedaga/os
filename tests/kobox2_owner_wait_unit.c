/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "../userland/kobox2_adapter/owner_wait.h"

static struct ph_owner_wait shared;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t progress = PTHREAD_COND_INITIALIZER;
static unsigned generation, completed;

static void *waiter(void *unused) {
    (void)unused;
    assert(!pthread_mutex_lock(&lock));
    for (unsigned step = 0; step < 1000; ++step) {
        while (generation == step) {
            unsigned observed = ph_owner_wait_begin(&shared);
            assert(!pthread_cond_broadcast(&progress));
            assert(!pthread_mutex_unlock(&lock));
            const struct timespec timeout = { .tv_sec = 5 };
            int result = syscall(SYS_futex, &shared.sequence, FUTEX_WAIT_PRIVATE,
                                 observed, &timeout, NULL, 0);
            assert(result == 0 || (result == -1 && (errno == EAGAIN || errno == EINTR)));
            assert(!pthread_mutex_lock(&lock));
            ph_owner_wait_end(&shared);
        }
        ++completed;
        assert(!pthread_cond_broadcast(&progress));
    }
    assert(!pthread_mutex_unlock(&lock));
    return NULL;
}

static void concurrent_waiters(void) {
    pthread_t threads[2];

    for (unsigned i = 0; i < 2; ++i)
        assert(!pthread_create(&threads[i], NULL, waiter, NULL));
    assert(!pthread_mutex_lock(&lock));
    for (unsigned step = 0; step < 1000; ++step) {
        while (shared.waiting != 2)
            assert(!pthread_cond_wait(&progress, &lock));
        ++generation;
        assert(ph_owner_wait_publish(&shared));
        assert(!pthread_mutex_unlock(&lock));
        assert(syscall(SYS_futex, &shared.sequence, FUTEX_WAKE_PRIVATE,
                       INT_MAX, NULL, NULL, 0) >= 0);
        assert(!pthread_mutex_lock(&lock));
        while (completed != 2 * generation)
            assert(!pthread_cond_wait(&progress, &lock));
    }
    assert(shared.waiting == 0);
    assert(!ph_owner_wait_publish(&shared));
    assert(!pthread_mutex_unlock(&lock));
    for (unsigned i = 0; i < 2; ++i)
        assert(!pthread_join(threads[i], NULL));
}

int main(void) {
    struct ph_owner_wait wait = { .sequence = ATOMIC_VAR_INIT(0) };

    /* Publication before registration is visible without issuing a wake. */
    assert(!ph_owner_wait_publish(&wait));
    assert(atomic_load(&wait.sequence) == 1);
    unsigned entry = ph_owner_wait_begin(&wait);
    assert(entry == 1 && wait.waiting == 1);

    /* Publication after registration, before native sleep, invalidates the
     * futex's expected value. The same helper must cover cpu_enter and cpu_wait.
     */
    assert(ph_owner_wait_publish(&wait));
    assert(atomic_load(&wait.sequence) != entry);
    unsigned idle = ph_owner_wait_begin(&wait);
    assert(idle == 2 && wait.waiting == 2);
    ph_owner_wait_end(&wait);
    assert(wait.waiting == 1);
    assert(ph_owner_wait_publish(&wait));
    assert(atomic_load(&wait.sequence) != idle);
    ph_owner_wait_end(&wait);
    assert(wait.waiting == 0);
    assert(!ph_owner_wait_publish(&wait));

    /* A spurious/interrupted native wait unregisters under the lock. If the
     * domain predicate is still false, its next wait must register again.
     */
    unsigned observed = ph_owner_wait_begin(&wait);
    ph_owner_wait_end(&wait);
    assert(!ph_owner_wait_publish(&wait));
    assert(atomic_load(&wait.sequence) != observed);
    observed = ph_owner_wait_begin(&wait);
    assert(ph_owner_wait_publish(&wait));
    assert(atomic_load(&wait.sequence) != observed);
    ph_owner_wait_end(&wait);

    /* Keep the existing unsigned sequence wrap semantics. One publication
     * across wrap still rejects the armed waiter's previous expected value.
     */
    atomic_store(&wait.sequence, UINT_MAX);
    observed = ph_owner_wait_begin(&wait);
    assert(observed == UINT_MAX);
    assert(ph_owner_wait_publish(&wait));
    assert(atomic_load(&wait.sequence) == 0);
    ph_owner_wait_end(&wait);
    assert(!wait.waiting);
    concurrent_waiters();
    puts("KOBOX2_OWNER_WAIT_UNIT_OK");
    return 0;
}
