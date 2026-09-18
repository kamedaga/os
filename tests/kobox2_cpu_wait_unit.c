/* SPDX-License-Identifier: MIT */
/* Real adapter wait/notify and deferred delivery, deterministic wake races. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../userland/kobox2_adapter/host.h"

static struct ph_task owner, sender, *current_task;
static unsigned waits, wakes, signals, delivered;
static struct ph_task *test_current_task(void) { return current_task; }
#define ph_current_task test_current_task
#include "../userland/kobox2_adapter/native.c"
#include "../userland/kobox2_adapter/machine.c"

_Noreturn void ph_fail(const char *file, unsigned line, uint64_t result) {
    fprintf(stderr, "%s:%u result=%llu\n", file, line, (unsigned long long)result);
    abort();
}
void ph_lock(atomic_uint *lock) { assert(atomic_exchange(lock, 1) == 0); }
void ph_unlock(atomic_uint *lock) { assert(atomic_exchange(lock, 0) == 1); }
void ph_wake(atomic_uint *word) { assert(word == &cpus[0].owner_sequence); ++wakes; }
long pacha_syscall2(uint64_t nr, uint64_t fd, uint64_t signo) {
    assert(nr == PACHA_THREAD_SYSCALL_SIGNAL && fd == 17 && signo == PH_NOTIFICATION_SIGNAL);
    ++signals;
    return 0;
}
static void notify(void) {
    current_task = &sender;
    assert(cpu_notify(0, KOBOX_LINUX_TASK_CONTROL_EVENT) == 0);
    current_task = &owner;
}
void ph_wait(atomic_uint *word, unsigned expected) {
    assert(word == &cpus[0].owner_sequence && cpus[0].owner_waiting);
    assert(atomic_load(&owner.notification_mask) == PH_NOTIFICATION_MASK);
    ++waits;
    /* Notifications arrive after unlock but before the futex checks its
     * value. Both counts must survive even though the native hint coalesces. */
    notify();
    notify();
    assert(atomic_load(word) != expected);
    assert(atomic_load(&owner.notification_deferred) == 1 && signals == 0);
}
static void dispatch_notification(uint32_t cpu, enum kobox_linux_task_notification kind,
                                  uint64_t count) {
    assert(cpu == 0 && kind == KOBOX_LINUX_TASK_CONTROL_EVENT);
    assert(current_task == &owner && !cpus[0].owner_waiting);
    assert(atomic_load(&cpus[0].owner_lock) == 0);
    delivered += (unsigned)count;
}
static void reset(void) {
    cpus[0] = (struct ph_cpu){0};
    owner = (struct ph_task){.fd = 17, .logical_cpu = 0};
    sender = (struct ph_task){.logical_cpu = -1};
    waits = wakes = signals = delivered = 0;
    assert(kobox_machine_domain_init(&cpus[0].domain) == 0);
    assert(kobox_machine_domain_enter(&cpus[0].domain, &owner) == 0);
    linux_notification_dispatch = dispatch_notification;
    current_task = &owner;
}
int main(void) {
    uint64_t sequence;
    reset();
    assert(cpu_wait(0, 0, &sequence) == 0);
    assert(sequence == 2 && waits == 1 && wakes == 2 && delivered == 2 && !signals);

    reset();
    atomic_store(&owner.notification_mask, PH_NOTIFICATION_MASK);
    assert(cpu_wait(0, 0, &sequence) == 0);
    assert(!delivered && atomic_load(&owner.notification_deferred) == 1);
    assert(ph_notifications_restore(0) == 0 && delivered == 2);

    reset();
    assert(irq_disable(0) == 0);
    assert(cpu_wait(0, 0, &sequence) == 0 && !delivered);
    assert(irq_enable(0) == 0 && delivered == 2);

    reset();
    notify(); /* A running owner must still receive an asynchronous signal. */
    assert(signals == 1 && !atomic_load(&owner.notification_deferred));
    assert(cpu_wait(0, 0, &sequence) == 0 && !waits && sequence == 1);
    ph_dispatch_notifications();
    assert(delivered == 1);
    puts("kobox2 CPU wait: PASS wake-before-wait, coalescing, masks, running-owner signal");
}
