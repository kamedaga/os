/* SPDX-License-Identifier: MIT */
/* Real adapter wait/notify, deferred delivery and fiber switches. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
/* No native FS here: thread words go through test_current_task(). */
#define PH_TLS_TEST_HOOKS 1
#include "../userland/kobox2_adapter/host.h"

/* One CPU thread; Linux tasks are fibers on it, as in the sandbox. A
 * notification sender is a separate helper thread. */
static struct ph_task thread, sender, *current_task;
static struct ph_fiber owner, *successor;
static unsigned waits, wakes, signals, delivered, successor_runs;
static struct ph_task *test_current_task(void) { return current_task; }
struct ph_task *ph_test_current_task(void) { return test_current_task(); }
#define ph_current_task test_current_task
#include "../userland/kobox2_adapter/native.c"
#include "../userland/kobox2_adapter/machine.c"

_Noreturn void ph_fail(const char *file, unsigned line, uint64_t result) {
    fprintf(stderr, "%s:%u result=%llu\n", file, line, (unsigned long long)result);
    abort();
}
void ph_exception(struct pacha_native_fault_frame *frame) {
    (void)frame;
    abort();
}
void ph_lock(atomic_uint *lock) { assert(atomic_exchange(lock, 1) == 0); }
void ph_unlock(atomic_uint *lock) { assert(atomic_exchange(lock, 0) == 1); }
void ph_wake(atomic_uint *word) { assert(word == &cpus[0].owner_wait.sequence); ++wakes; }
void *ph_alloc(size_t size) {
    void *memory = aligned_alloc(PH_PAGE_SIZE, (size + PH_PAGE_SIZE - 1) & ~(PH_PAGE_SIZE - 1));
    assert(memory);
    return memory;
}
void ph_free(void *base, size_t size) { (void)size; free(base); }
long pacha_syscall3(uint64_t nr, uint64_t a, uint64_t b, uint64_t c) {
    /* Only the fiber guard page; the host test does not enforce it. */
    assert(nr == PACHA_VM_SYSCALL_MPROTECT && a && b == PH_PAGE_SIZE && !c);
    return 0;
}
/* Native thread operations: no origin thread exists in this test. */
long pacha_syscall1(uint64_t nr, uint64_t a) { (void)nr; (void)a; abort(); }
long pacha_syscall4(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)nr; (void)a; (void)b; (void)c; (void)d;
    abort();
}
long pacha_syscall6(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d,
    uint64_t e, uint64_t f) {
    (void)nr; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    abort();
}
long pacha_syscall2(uint64_t nr, uint64_t fd, uint64_t signo) {
    assert(nr == PACHA_THREAD_SYSCALL_SIGNAL && fd == 17 && signo == PH_NOTIFICATION_SIGNAL);
    ++signals;
    return 0;
}
static void notify(void) {
    current_task = &sender;
    assert(cpu_notify(0, KOBOX_LINUX_TASK_CONTROL_EVENT) == 0);
    current_task = &thread;
}
void ph_wait(atomic_uint *word, unsigned expected) {
    assert(word == &cpus[0].owner_wait.sequence && cpus[0].owner_waiting);
    assert(cpus[0].owner_wait.waiting == 1);
    assert(atomic_load(&thread.notification_mask) == PH_NOTIFICATION_MASK);
    ++waits;
    /* Notifications arrive after unlock but before the futex checks its
     * value. Both counts must survive even though the native hint coalesces. */
    notify();
    notify();
    assert(atomic_load(word) != expected);
    assert(atomic_load(&thread.notification_deferred) == 1 && signals == 0);
}
static void dispatch_notification(uint32_t cpu, enum kobox_linux_task_notification kind,
                                  uint64_t count) {
    assert(cpu == 0 && kind == KOBOX_LINUX_TASK_CONTROL_EVENT);
    assert(current_task == &thread && !cpus[0].owner_waiting);
    assert(atomic_load(&cpus[0].owner_lock) == 0);
    delivered += (unsigned)count;
}
static void reset(void) {
    cpus[0] = (struct ph_cpu){0};
    thread = (struct ph_task){.fd = 17};
    sender = (struct ph_task){0};
    owner = (struct ph_fiber){.logical_cpu = 0, .state = PH_FIBER_RUNNING, .bound = true};
    thread.self = &thread;
    thread.fiber = &owner;
    cpus[0].thread = &thread;
    waits = wakes = signals = delivered = successor_runs = 0;
    assert(kobox_machine_domain_init(&cpus[0].domain) == 0);
    assert(kobox_machine_domain_enter(&cpus[0].domain, &owner) == 0);
    linux_notification_dispatch = dispatch_notification;
    current_task = &thread;
}
static void irq_enable_publication(void) {
    struct pacha_native_signal_frame frame = {
        .magic = PACHA_PROCESS_SIGNAL_FRAME_MAGIC,
        .signo = PH_NOTIFICATION_SIGNAL,
    };
    uint64_t previous;

    reset();
    assert(!ph_notifications_save(&previous) && !previous);
    assert(!irq_disable(0) && !irq_enable(0));
    assert(!delivered && !kobox_machine_domain_pending(&cpus[0].domain));
    /* Publish after the empty enable check but before its outer unmask. Use
     * the real signal body: the syscall stub alone does not deliver upcalls.
     * Two counts must survive a single deferred hint. */
    notify();
    ph_notification_body(&frame);
    notify();
    ph_notification_body(&frame);
    assert(signals == 2 && !delivered && thread.notification_deferred == 1);
    assert(!ph_notifications_restore(previous) && delivered == 2);
    assert(!thread.notification_deferred &&
           !kobox_machine_domain_pending(&cpus[0].domain));

    reset();
    assert(!ph_notifications_save(&previous));
    assert(!irq_disable(0) && !irq_disable(0));
    notify();
    ph_notification_body(&frame);
    assert(!irq_enable(0) && !delivered);
    assert(!irq_enable(0) && delivered == 1);
    assert(!ph_notifications_restore(previous) && delivered == 1);
    assert(!thread.notification_deferred);
}
/* A NEW fiber runs as port.c's task_bootstrap would: it must enter the CPU
 * the switch handed over, then switch back as Linux would. */
static void *successor_entry(void *argument) {
    struct pacha_native_signal_frame frame = {
        .magic = PACHA_PROCESS_SIGNAL_FRAME_MAGIC,
        .signo = PH_NOTIFICATION_SIGNAL,
    };

    assert(argument == &owner && ph_current_fiber() == successor);
    assert(successor->state == PH_FIBER_RUNNING && owner.state == PH_FIBER_SWITCHED);
    assert(owner.logical_cpu == -1 && successor->logical_cpu == -1);
    assert(!cpus[0].owner_lock && cpus[0].domain.owner == successor &&
           cpus[0].domain.handoff_released);
    /* The switch is still masked: a notification arriving before the
     * successor enters its CPU cannot run Linux; cpu_enter must drain it. */
    assert(atomic_load(&thread.notification_mask) == PH_NOTIFICATION_MASK);
    notify();
    ph_notification_body(&frame);
    assert(signals == 1 && !delivered && thread.notification_deferred &&
           kobox_machine_domain_pending(&cpus[0].domain));
    assert(!cpu_enter(0, successor));
    assert(delivered == 1 && !kobox_machine_domain_pending(&cpus[0].domain));
    assert(successor->logical_cpu == 0 && !thread.notification_deferred);
    ++successor_runs;
    /* Linux switches with IRQ entry masked; port.c holds IRQs disabled. */
    uint64_t mask;
    assert(!ph_notifications_save(&mask) && !mask);
    assert(!cpu_switch(0, successor, &owner, 0));
    abort(); /* owner never switches back to this fiber */
}

static void switch_handoff_gap(void) {
    reset();
    successor = ph_fiber_create(successor_entry, &owner);
    assert(successor->state == PH_FIBER_NEW && !((uintptr_t)successor->rsp & 15));
    uint64_t mask;
    assert(!ph_notifications_save(&mask) && !mask);
    assert(!cpu_switch(0, &owner, successor, 0));
    /* Resumed on the same thread by the successor's switch. port.c now
     * enters the CPU the resuming switch handed back. */
    assert(successor_runs == 1 && ph_current_fiber() == &owner);
    assert(owner.state == PH_FIBER_RUNNING && successor->state == PH_FIBER_SWITCHED);
    assert(cpus[0].domain.owner == &owner && owner.logical_cpu == -1);
    assert(!cpu_enter(0, &owner) && owner.logical_cpu == 0);
    assert(!ph_notifications_restore(mask) && !thread.notification_mask);
    /* Its last switch done, the stack is idle; Linux would release it now. */
    successor->state = PH_FIBER_DEAD;
    ph_fiber_destroy(successor);
}

int main(void) {
    uint64_t sequence;
    irq_enable_publication();
    reset();
    assert(cpu_wait(0, 0, &sequence) == 0);
    assert(sequence == 2 && waits == 1 && wakes == 2 && delivered == 2 && !signals);
    assert(!cpus[0].owner_wait.waiting);

    reset();
    atomic_store(&thread.notification_mask, PH_NOTIFICATION_MASK);
    assert(cpu_wait(0, 0, &sequence) == 0);
    assert(!delivered && atomic_load(&thread.notification_deferred) == 1);
    assert(ph_notifications_restore(0) == 0 && delivered == 2);

    reset();
    assert(irq_disable(0) == 0);
    assert(cpu_wait(0, 0, &sequence) == 0 && !delivered);
    assert(irq_enable(0) == 0 && delivered == 2);

    reset();
    notify(); /* A running owner must still receive an asynchronous signal. */
    assert(signals == 1 && !atomic_load(&thread.notification_deferred));
    assert(cpu_wait(0, 0, &sequence) == 0 && !waits && sequence == 1);
    ph_dispatch_notifications();
    assert(delivered == 1);
    switch_handoff_gap();
    puts("kobox2 CPU wait: PASS wake-before-wait, coalescing, masks, running-owner signal, empty-enable publication, nested IRQ, fiber switch handoff-gap drain");
}
