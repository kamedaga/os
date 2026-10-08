/* SPDX-License-Identifier: MIT */
/* Count conservation under concurrent publication and empty IRQ scans. */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include "../kobox2/linux-sandbox/kobox/machine/domain.h"

static struct kobox_machine_domain domain;
static atomic_bool published;
enum { ROUNDS = 10000 };

static void *producer(void *unused)
{
    (void)unused;
    /* One producer needs no execution lock against another producer. The
     * consumer only claims counters; accepting and owner never change here. */
    for (unsigned i = 0; i < ROUNDS; ++i) {
        for (unsigned kind = 0; kind < KOBOX_MACHINE_NOTIFICATION_COUNT; ++kind)
            assert(kobox_machine_domain_notify(&domain, kind) == KOBOX_MACHINE_OK);
        if (!(i % 128)) sched_yield();
    }
    atomic_store_explicit(&published, true, memory_order_release);
    return NULL;
}

int main(void)
{
    uint64_t total[KOBOX_MACHINE_NOTIFICATION_COUNT] = {0};
    assert(kobox_machine_domain_init(&domain) == KOBOX_MACHINE_OK);
    assert(kobox_machine_domain_enter(&domain, &domain) == KOBOX_MACHINE_OK);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, producer, NULL));
    do {
        enum kobox_machine_notification kind;
        uint64_t count;
        if (kobox_machine_domain_irq_take(&domain, &kind, &count)) {
            assert(kind < KOBOX_MACHINE_NOTIFICATION_COUNT && count);
            total[kind] += count;
            assert(kobox_machine_domain_irq_return(&domain));
        } else {
            assert(!count);
            sched_yield();
        }
    } while (!atomic_load_explicit(&published, memory_order_acquire) ||
             kobox_machine_domain_pending(&domain));
    assert(!pthread_join(thread, NULL));
    for (unsigned kind = 0; kind < KOBOX_MACHINE_NOTIFICATION_COUNT; ++kind)
        assert(total[kind] == ROUNDS);
    assert(atomic_load(&domain.sequence) == ROUNDS * KOBOX_MACHINE_NOTIFICATION_COUNT);
    puts("machine domain publication race: PASS all kinds, counts conserved");
    return 0;
}
