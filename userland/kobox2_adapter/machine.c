/* SPDX-License-Identifier: MIT */
#include "host.h"
#include "tail_trace.h"
#include "owner_wait.h"
#include "arch/x86_64/fp_entry.h"
#if defined(PH_SCHED_DIAGNOSTIC) && PH_SCHED_DIAGNOSTIC
#include "scheduler_diagnostic.h"
#endif

#define NANOSECONDS_PER_SECOND UINT64_C(1000000000)
#define CPU_STOP_TIMEOUT_NS (2 * NANOSECONDS_PER_SECOND)

struct ph_cpu {
    struct kobox_machine_domain domain;
    atomic_uint owner_lock;
    struct ph_owner_wait owner_wait;
    bool owner_waiting; /* Under owner_lock; native Linux entry is deferred. */
    /* The native thread running this logical CPU's fibers. The domain owner is
     * the fiber; notifications go to this thread. Published under owner_lock
     * by its first cpu_enter, before the domain has an owner. */
    struct ph_task *thread;
    /* Never published: a stopped CPU thread waits on it forever. */
    atomic_uint stopped_word;

    /* Timer programming and expiry publication use a separate lock.
     * Lock order is timer_lock -> owner_lock, never the reverse. */
    atomic_uint timer_lock;
    pacha_fd_t timer_fd;
    pacha_fd_t stop_fd;
    uint64_t deadline_ns;
    struct ph_task *timer_task;
#if defined(PH_SCHED_DIAGNOSTIC) && PH_SCHED_DIAGNOSTIC
    atomic_ullong diagnostic_timer_arms, diagnostic_timer_expiries;
    atomic_ullong diagnostic_notifies, diagnostic_notify_returns;
    atomic_ullong diagnostic_callbacks, diagnostic_callback_returns;
#endif
};

static struct ph_cpu cpus[PH_CPU_COUNT];
static kobox_linux_task_notification_fn linux_notification_dispatch;

static int monotonic(uint64_t *nanoseconds);

/* All callers hold owner_lock and defer Linux notification entry before taking it.
 * Waiters snapshot the sequence while locked, then futex-wait after unlocking. */
static void wake_cpu_waiters(struct ph_cpu *cpu) {
    if (ph_owner_wait_publish(&cpu->owner_wait))
        ph_wake(&cpu->owner_wait.sequence);
}

/* Linux wakes ordinary tasks through its runqueue and reaches them by
 * switching to them, so a native wake reaches only a NEW fiber: the secondary
 * idle task (kick) or a task copy_process abandoned (abort). The adapter
 * cannot tell them apart, so both start a native thread on the fiber's own
 * stack; an aborted task_bootstrap returns at once and that thread exits. */
static int task_wake(void *opaque) {
    uint64_t previous_mask;

    PH_OK(ph_notifications_save(&previous_mask));
    (void)ph_fiber_thread_start(opaque);
    PH_OK(ph_notifications_restore(previous_mask));
    return 0;
}

/* The contract still requires the entry; a fiber is suspended by switching. */
static int task_park(void *opaque) {
    (void)opaque;
    ph_fail(__FILE__, __LINE__, 0);
}

static int task_create(void **out, void *(*entry)(void *), void *argument) {
    uint64_t previous_mask;

    PH_OK(ph_notifications_save(&previous_mask));
    *out = ph_fiber_create(entry, argument);
    PH_OK(ph_notifications_restore(previous_mask));
    return 0;
}

static int task_bind(void **out) {
    struct ph_fiber *fiber = ph_current_fiber();

    PH_CHECK(fiber && fiber->bound);
    *out = fiber;
    return 0;
}

/* Linux exception handling also runs on the fiber stack (exception.c), so
 * the native fault stack never holds a Linux object. */
static uint8_t task_on_stack(void *opaque, const void *object) {
    const struct ph_fiber *fiber = opaque;
    uintptr_t address = (uintptr_t)object;
    return address - (uintptr_t)fiber->stack < fiber->stack_size;
}

static int task_destroy_current(void *task) {
    PH_CHECK(task == ph_current_fiber() && ((struct ph_fiber *)task)->bound);
    return 0;
}

/* An exited task's last switch completed before Linux cleared on_cpu, so its
 * stack is idle. An origin fiber's thread may still run on it: the native
 * join inside ph_fiber_destroy waits for that thread first. */
static int task_join(void *task) {
    uint64_t previous_mask;

    PH_OK(ph_notifications_save(&previous_mask));
    ph_fiber_destroy(task);
    PH_OK(ph_notifications_restore(previous_mask));
    return 0;
}

static int cpu_enter(uint32_t index, void *opaque) {
    struct ph_fiber *fiber = opaque;

    PH_CHECK(index < PH_CPU_COUNT && fiber == ph_current_fiber() && fiber->logical_cpu < 0);
    struct ph_cpu *cpu = &cpus[index];

    PH_OK(ph_notifications_save(&fiber->saved_notification_mask));
    struct ph_task *thread = ph_current_task();
    ph_lock(&cpu->owner_lock);
    /* Only this CPU's thread ever runs its fibers: a switch resumes the next
     * fiber on the switching thread. Entry therefore never waits: the domain
     * is free (first entry) or already handed to this fiber. */
    PH_CHECK(!cpu->thread || cpu->thread == thread);
    cpu->thread = thread;
    PH_OK(kobox_machine_domain_enter(&cpu->domain, fiber));
    fiber->logical_cpu = (int)index;
    ph_unlock(&cpu->owner_lock);

    /* Drain notifications published while the domain had no running owner. */
    PH_OK(ph_notifications_restore(0));
    ph_dispatch_notifications();
    return 0;
}

static int cpu_leave(uint32_t index) {
    struct ph_fiber *fiber = ph_current_fiber();
    uint64_t previous_mask;

    PH_CHECK(index < PH_CPU_COUNT && fiber->logical_cpu == (int)index);
    struct ph_cpu *cpu = &cpus[index];

    PH_OK(ph_notifications_save(&previous_mask));
    PH_CHECK(atomic_load(cpu->domain.irq_depth) == 0);
    ph_lock(&cpu->owner_lock);
    kobox_machine_domain_release(&cpu->domain);
    fiber->logical_cpu = -1;
    /* Leaving is the CPU going offline: its thread exits next and is joined.
     * A later online starts a new thread, which the next cpu_enter adopts. */
    cpu->thread = NULL;
    wake_cpu_waiters(cpu);
    ph_unlock(&cpu->owner_lock);
    PH_OK(ph_notifications_restore(fiber->saved_notification_mask));
    return 0;
}

static bool on_fault_stack(void) {
    char marker;
    uintptr_t base = (uintptr_t)ph_current_task()->fault_stack;
    return (uintptr_t)&marker - base < PH_THREAD_STACK_SIZE;
}

/* Linux calls this with IRQs disabled, preempt_count == 2 and the runqueue
 * locked (port.c checks). It returns when a CPU thread switches back to
 * previous, possibly for another logical CPU: port.c then rereads resume_cpu
 * and calls cpu_enter for it. */
static int cpu_switch(uint32_t index, void *previous_opaque, void *next_opaque, uint8_t exiting) {
    struct ph_fiber *previous = previous_opaque, *next = next_opaque;
    uint64_t previous_mask;

    /* Masked from here: no notification enters Linux and nothing migrates
     * until a resumed fiber restores its own mask. */
    PH_OK(ph_notifications_save(&previous_mask));
    PH_CHECK(index < PH_CPU_COUNT && previous == ph_current_fiber() &&
        previous->logical_cpu == (int)index && next != previous);
    /* The native fault frame and stack belong to this thread; no other fiber
     * may run while they are in use (exception.c keeps Linux off them). */
    PH_CHECK(!on_fault_stack());
    PH_CHECK(next->state == PH_FIBER_NEW || next->state == PH_FIBER_SWITCHED);
    struct ph_cpu *cpu = &cpus[index];

    ph_lock(&cpu->owner_lock);
    PH_OK(kobox_machine_domain_handoff(&cpu->domain, previous, next));
    previous->logical_cpu = -1;
    kobox_machine_domain_handoff_finish(&cpu->domain);
    ph_unlock(&cpu->owner_lock);

    /* Linux's on_cpu keeps another CPU from resuming previous before the save
     * below: finish_task_switch clears it on next, after this switch. */
    next->state = PH_FIBER_RUNNING;
    ph_set_current_fiber(next);
    if (exiting) {
        uint64_t discarded;

        /* Never resumed; task_join releases the stack once Linux has
         * cleared on_cpu, which happens after this switch. */
        previous->state = PH_FIBER_DEAD;
        ph_fiber_switch(&discarded, next->rsp);
        ph_fail(__FILE__, __LINE__, 0);
    }
    previous->state = PH_FIBER_SWITCHED;
    ph_fiber_switch(&previous->rsp, next->rsp);
    /* Resumed, possibly by another CPU thread: cpu is stale here. The resuming
     * switch already handed the domain to this fiber; cpu_enter claims it. */
    (void)previous_mask;
    PH_OK(ph_notifications_restore(previous->saved_notification_mask));
    return 0;
}

static int cpu_wait(uint32_t index, uint64_t observed, uint64_t *sequence_out) {
    uint64_t previous_mask;

    PH_CHECK(index < PH_CPU_COUNT && ph_current_cpu() == (int)index);
    struct ph_cpu *cpu = &cpus[index];

    PH_OK(ph_notifications_save(&previous_mask));
    ph_lock(&cpu->owner_lock);
    while (kobox_machine_domain_should_wait(&cpu->domain, observed)) {
        PH_CHECK(cpu->owner_wait.waiting != UINT32_MAX);
        unsigned owner_sequence = ph_owner_wait_begin(&cpu->owner_wait);

        cpu->owner_waiting = true;
        ph_unlock(&cpu->owner_lock);
        ph_wait(&cpu->owner_wait.sequence, owner_sequence);
        ph_lock(&cpu->owner_lock);
        PH_CHECK(cpu->owner_wait.waiting != 0);
        ph_owner_wait_end(&cpu->owner_wait);
        cpu->owner_waiting = false;
    }
    *sequence_out = atomic_load_explicit(&cpu->domain.sequence, memory_order_acquire);
    ph_unlock(&cpu->owner_lock);
    PH_OK(ph_notifications_restore(previous_mask));
    return 0;
}

static enum kobox_machine_notification notification_kind(
    enum kobox_linux_task_notification kind) {
    switch (kind) {
    case KOBOX_LINUX_TASK_CLOCKEVENT:
        return KOBOX_MACHINE_TICK;
    case KOBOX_LINUX_TASK_RESCHEDULE:
        return KOBOX_MACHINE_RESCHEDULE;
    case KOBOX_LINUX_TASK_CALL_FUNCTION:
        return KOBOX_MACHINE_CALL_FUNCTION;
    case KOBOX_LINUX_TASK_DEVICE_IRQ:
        return KOBOX_MACHINE_DEVICE_IRQ;
    case KOBOX_LINUX_TASK_CONTROL_EVENT:
        return KOBOX_MACHINE_CONTROL_EVENT;
    case KOBOX_LINUX_TASK_VM_EVENT:
        return KOBOX_MACHINE_VM_EVENT;
    }
    ph_fail(__FILE__, __LINE__, kind);
}

static int cpu_notify(uint32_t index, enum kobox_linux_task_notification kind) {
    uint64_t previous_mask;

    PH_CHECK(index < PH_CPU_COUNT);
    PH_OK(ph_notifications_save(&previous_mask));
    struct ph_cpu *cpu = &cpus[index];
#if defined(PH_SCHED_DIAGNOSTIC) && PH_SCHED_DIAGNOSTIC
    atomic_fetch_add_explicit(&cpu->diagnostic_notifies, 1, memory_order_relaxed);
#endif

    ph_lock(&cpu->owner_lock);
    PH_OK(kobox_machine_domain_notify(&cpu->domain, notification_kind(kind)));
    /* A fiber owns the domain (possibly mid-switch); its CPU thread runs it. */
    struct ph_task *thread = cpu->domain.owner ? cpu->thread : NULL;

    /* cpu_wait defers Linux entry until after releasing owner_lock. Its
     * sequence-checked wait supplies the wake; mark the same deferred hint
     * a native signal would set, without entering and returning from one.
     * A running owner still needs an asynchronous signal. */
    if (thread && cpu->owner_waiting)
        atomic_store_explicit(&thread->notification_deferred, 1, memory_order_release);
    wake_cpu_waiters(cpu);
    /* Keep owner_lock through SIGNAL: the domain cannot be released and its
     * thread cannot exit between lookup and delivery. The domain retains the
     * actual event count. */
    if (thread != NULL && !cpu->owner_waiting) {
        PH_OK(pacha_syscall2(PACHA_THREAD_SYSCALL_SIGNAL, thread->fd, PH_NOTIFICATION_SIGNAL));
    }
    ph_unlock(&cpu->owner_lock);
    PH_OK(ph_notifications_restore(previous_mask));
#if defined(PH_SCHED_DIAGNOSTIC) && PH_SCHED_DIAGNOSTIC
    atomic_fetch_add_explicit(&cpu->diagnostic_notify_returns, 1, memory_order_relaxed);
#endif
    return 0;
}

static _Noreturn void acknowledge_cpu_stop(struct ph_cpu *cpu) {
    atomic_store_explicit(&cpu->domain.stopped, true, memory_order_release);
    for (;;)
        ph_wait(&cpu->stopped_word, 0);
}

void ph_dispatch_notifications(void) {
    static const enum kobox_linux_task_notification linux_kinds[] = {
        [KOBOX_MACHINE_TICK] = KOBOX_LINUX_TASK_CLOCKEVENT,
        [KOBOX_MACHINE_RESCHEDULE] = KOBOX_LINUX_TASK_RESCHEDULE,
        [KOBOX_MACHINE_CALL_FUNCTION] = KOBOX_LINUX_TASK_CALL_FUNCTION,
        [KOBOX_MACHINE_DEVICE_IRQ] = KOBOX_LINUX_TASK_DEVICE_IRQ,
        [KOBOX_MACHINE_CONTROL_EVENT] = KOBOX_LINUX_TASK_CONTROL_EVENT,
        [KOBOX_MACHINE_VM_EVENT] = KOBOX_LINUX_TASK_VM_EVENT,
    };
    uint64_t previous_mask;

    PH_OK(ph_notifications_save(&previous_mask));
    for (;;) {
        int index = ph_current_cpu();

        if (index < 0) {
            break;
        }
        struct ph_cpu *cpu = &cpus[index];
        enum kobox_machine_notification kind;
        uint64_t count;

        if (atomic_load_explicit(&cpu->domain.stop_requested, memory_order_acquire)) {
            acknowledge_cpu_stop(cpu);
        }
        if (!kobox_machine_domain_irq_take(&cpu->domain, &kind, &count)) {
            break;
        }

        /* irq_take masks logical IRQs. Native delivery stays enabled while
         * Linux runs, allowing nested interrupts when Linux enables IRQs. */
        PH_OK(ph_notifications_restore(0));
#if defined(PH_SCHED_DIAGNOSTIC) && PH_SCHED_DIAGNOSTIC
        struct ph_cpu *entered_cpu = cpu;
        atomic_fetch_add_explicit(&entered_cpu->diagnostic_callbacks, 1, memory_order_relaxed);
#endif
        uint64_t tail_begin = ph_tail_begin();
        linux_notification_dispatch((uint32_t)index, linux_kinds[kind], count);
        ph_tail_end(3, (uintptr_t)kind, tail_begin);
#if defined(PH_SCHED_DIAGNOSTIC) && PH_SCHED_DIAGNOSTIC
        atomic_fetch_add_explicit(&entered_cpu->diagnostic_callback_returns, 1, memory_order_relaxed);
#endif

        /* Callback execution can migrate this task. IRQ return belongs to
         * the current domain, not necessarily the domain used for entry. */
        uint64_t callback_mask;
        PH_OK(ph_notifications_save(&callback_mask));
        index = ph_current_cpu();
        PH_CHECK(index >= 0 && kobox_machine_domain_irq_return(&cpus[index].domain));
    }
    PH_OK(ph_notifications_restore(previous_mask));
}

#include "irq_callbacks.h"

static uint64_t notification_sequence(uint32_t index) {
    PH_CHECK(index < PH_CPU_COUNT);
    return atomic_load(&cpus[index].domain.sequence);
}

#include "clock_callbacks.h"

static int cpu_stop(uint32_t index) {
    uint64_t started_at;
    uint64_t now;

    PH_CHECK(index < PH_CPU_COUNT && ph_current_cpu() != (int)index);
    atomic_store_explicit(&cpus[index].domain.stop_requested, true, memory_order_release);
    PH_OK(cpu_notify(index, KOBOX_LINUX_TASK_RESCHEDULE));
    PH_OK(monotonic(&started_at));

    while (!atomic_load_explicit(&cpus[index].domain.stopped, memory_order_acquire)) {
        PH_OK(monotonic(&now));
        PH_CHECK(now - started_at < CPU_STOP_TIMEOUT_NS);
    }
    return 0;
}

static int clockevent_arm(uint32_t index, uint64_t deadline_ns) {
    uint64_t previous_mask;
    uint64_t timer_spec[4] = {
        0, 0, /* One-shot: interval seconds/nanoseconds. */
        deadline_ns / NANOSECONDS_PER_SECOND,
        deadline_ns % NANOSECONDS_PER_SECOND,
    };

    PH_CHECK(index < PH_CPU_COUNT);
    struct ph_cpu *cpu = &cpus[index];

    PH_OK(ph_notifications_save(&previous_mask));
    ph_lock(&cpu->timer_lock);
    cpu->deadline_ns = deadline_ns;
    uint64_t tail_begin = ph_tail_begin();
    PH_OK(pacha_syscall4(PACHA_FD_SYSCALL_TIMERFD_SETTIME, cpu->timer_fd,
        PACHA_TIMERFD_ABSTIME, (uintptr_t)timer_spec, 0));
    ph_tail_end(4, index, tail_begin);
    ph_unlock(&cpu->timer_lock);
#if defined(PH_SCHED_DIAGNOSTIC) && PH_SCHED_DIAGNOSTIC
    atomic_fetch_add_explicit(&cpu->diagnostic_timer_arms, 1, memory_order_relaxed);
#endif
    PH_OK(ph_notifications_restore(previous_mask));
    return 0;
}

static int clockevent_cancel(uint32_t index) {
    return clockevent_arm(index, 0);
}

static void publish_expired_clockevent(uint32_t index) {
    struct ph_cpu *cpu = &cpus[index];
    uint64_t now;

    ph_lock(&cpu->timer_lock);
    PH_OK(monotonic(&now));

    /* A cancel/rearm may win after WAIT_MANY. Only the current deadline can
     * publish an interrupt; the core also rejects stale clockevent delivery. */
    if (cpu->deadline_ns != 0 && now >= cpu->deadline_ns) {
        uint64_t expirations;
        long bytes = pacha_syscall3(PACHA_FD_SYSCALL_READ, cpu->timer_fd,
            (uintptr_t)&expirations, sizeof(expirations));

        PH_CHECK(bytes == sizeof(expirations));
        cpu->deadline_ns = 0;
#if defined(PH_SCHED_DIAGNOSTIC) && PH_SCHED_DIAGNOSTIC
        atomic_fetch_add_explicit(&cpu->diagnostic_timer_expiries, 1, memory_order_relaxed);
#endif
        PH_OK(cpu_notify(index, KOBOX_LINUX_TASK_CLOCKEVENT));
    }
    ph_unlock(&cpu->timer_lock);
}

static void *timer_entry(void *opaque) {
    uint32_t index = (uint32_t)(uintptr_t)opaque;
    struct ph_cpu *cpu = &cpus[index];
    struct ph_wait_retained wait_set = {0};

    for (;;) {
        struct pacha_pollfd fds[2] = {
            {.fd = cpu->timer_fd, .events = PACHA_FD_EVENT_READABLE},
            {.fd = cpu->stop_fd, .events = PACHA_FD_EVENT_READABLE},
        };
        long ready_count = ph_wait_many_retained(&wait_set,
            fds, 2, PACHA_FD_WAIT_FOREVER);

        PH_CHECK(ready_count > 0 && ready_count <= 2);
        PH_CHECK(fds[0].revents != 0 || fds[1].revents != 0);
        PH_CHECK((fds[0].revents & ~PACHA_FD_EVENT_READABLE) == 0);
        PH_CHECK((fds[1].revents & ~PACHA_FD_EVENT_READABLE) == 0);
        if (fds[1].revents != 0) {
            return NULL;
        }
        publish_expired_clockevent(index);
    }
}

static int clockevent_stop(uint32_t index) {
    uint64_t previous_mask;
    uint64_t stop_event = 1;

    PH_CHECK(index < PH_CPU_COUNT);
    PH_OK(ph_notifications_save(&previous_mask));
    struct ph_cpu *cpu = &cpus[index];

    long bytes = pacha_syscall3(PACHA_FD_SYSCALL_WRITE, cpu->stop_fd,
        (uintptr_t)&stop_event, sizeof(stop_event));
    PH_CHECK(bytes == sizeof(stop_event));
    PH_OK(ph_thread_join(cpu->timer_task));
    PH_OK(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, cpu->timer_fd));
    PH_OK(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, cpu->stop_fd));
    PH_OK(ph_notifications_restore(previous_mask));
    return 0;
}

#if defined(PH_SCHED_DIAGNOSTIC) && PH_SCHED_DIAGNOSTIC
static int diagnostic_write(const char *record, size_t length) {
    PH_CHECK(length && length <= PH_SCHED_LOG_BYTES);
    return (int)pacha_syscall2(1, (uintptr_t)record, length);
}

static void diagnostic_snapshot(unsigned index, uint64_t now) {
    struct ph_cpu *cpu = &cpus[index];
    const char *labels[] = {" cpu=", " ns=", " owner_available=", " owner=",
        " owner_fd=", " irq_depth=", " owner_waiting=", " handoff_released=",
        " mask=", " deferred=", " permit=", " frame_valid=", " frame_sequence=",
        " rip=", " rsp=", " frame_mask=", " wait_word=", " wait_expected=",
        " timer_lock=", " timer_arms=", " timer_expiries=", " notify_enter=",
        " notify_return=", " callback_enter=", " callback_return=", " sequence=",
        " pending_tick=", " pending_resched=", " pending_call=", " pending_device=",
        " pending_control=", " pending_vm=", " owner_sequence=",
        " rdi=", " rsi=", " rax=", " rbx=", " stack_valid=",
        " stack0=", " stack1=", " stack2=", " stack3=",
        " stack4=", " stack5=", " stack6=", " stack7=",
        " stack8=", " stack9=", " stack10=", " stack11=",
        " stack12=", " stack13=", " stack14=", " stack15="};
    uint64_t values[sizeof(labels) / sizeof(*labels)] = {index, now};
    unsigned unlocked = 0;
    /* Never wait behind a stuck owner or dereference its unpinned task.
     * An acquired owner_lock pins the same owner lifetime as cpu_notify.
     * No logging, syscalls or Linux entry occurs while this snapshot holds it. */
    if (atomic_compare_exchange_strong_explicit(&cpu->owner_lock, &unlocked, 1,
            memory_order_acquire, memory_order_relaxed)) {
        /* The domain owner is a fiber; mask and frame state are its CPU
         * thread's, which outlives every owner of this domain. */
        struct ph_task *owner = cpu->domain.owner ? cpu->thread : NULL;
        values[2] = 1;
        values[3] = (uintptr_t)cpu->domain.owner;
        values[6] = cpu->owner_waiting;
        values[7] = cpu->domain.handoff_released;
        if (owner) {
            values[4] = owner->fd;
            values[8] = atomic_load_explicit(&owner->notification_mask, memory_order_acquire);
            values[9] = atomic_load_explicit(&owner->notification_deferred, memory_order_acquire);
            uint64_t before = atomic_load_explicit(&owner->diagnostic_frame_sequence,
                memory_order_acquire);
            values[13] = atomic_load_explicit(&owner->diagnostic_rip, memory_order_relaxed);
            values[14] = atomic_load_explicit(&owner->diagnostic_rsp, memory_order_relaxed);
            values[15] = atomic_load_explicit(&owner->diagnostic_frame_mask, memory_order_relaxed);
            values[33] = atomic_load_explicit(&owner->diagnostic_rdi, memory_order_relaxed);
            values[34] = atomic_load_explicit(&owner->diagnostic_rsi, memory_order_relaxed);
            values[35] = atomic_load_explicit(&owner->diagnostic_rax, memory_order_relaxed);
            values[36] = atomic_load_explicit(&owner->diagnostic_rbx, memory_order_relaxed);
            values[37] = atomic_load_explicit(&owner->diagnostic_stack_valid, memory_order_relaxed);
            for (unsigned i = 0; i < PH_SCHED_STACK_WORDS; ++i)
                values[38+i] = atomic_load_explicit(&owner->diagnostic_stack_words[i], memory_order_relaxed);
            atomic_thread_fence(memory_order_acquire);
            uint64_t after = atomic_load_explicit(&owner->diagnostic_frame_sequence,
                memory_order_acquire);
            values[11] = before && !(before & 1) && before == after;
            values[12] = after;
            if (!values[11]) {
                values[13] = values[14] = values[15] = 0;
                for (unsigned i = 33; i < sizeof(values) / sizeof(*values); ++i) values[i] = 0;
            }
            values[16] = atomic_load_explicit(&owner->diagnostic_wait_word, memory_order_acquire);
            values[17] = atomic_load_explicit(&owner->diagnostic_wait_expected, memory_order_relaxed);
        }
        ph_unlock(&cpu->owner_lock);
    }
    /* These counters are independent atomic observations, not a claimed
     * simultaneous Linux state. Explicit validity covers owner/frame data. */
    values[5] = atomic_load_explicit(cpu->domain.irq_depth, memory_order_acquire);
    values[18] = atomic_load_explicit(&cpu->timer_lock, memory_order_acquire);
    values[19] = atomic_load_explicit(&cpu->diagnostic_timer_arms, memory_order_relaxed);
    values[20] = atomic_load_explicit(&cpu->diagnostic_timer_expiries, memory_order_relaxed);
    values[21] = atomic_load_explicit(&cpu->diagnostic_notifies, memory_order_relaxed);
    values[22] = atomic_load_explicit(&cpu->diagnostic_notify_returns, memory_order_relaxed);
    values[23] = atomic_load_explicit(&cpu->diagnostic_callbacks, memory_order_relaxed);
    values[24] = atomic_load_explicit(&cpu->diagnostic_callback_returns, memory_order_relaxed);
    values[25] = atomic_load_explicit(&cpu->domain.sequence, memory_order_acquire);
    for (unsigned i = 0; i < KOBOX_MACHINE_NOTIFICATION_COUNT; ++i)
        values[26 + i] = atomic_load_explicit(&cpu->domain.pending[i], memory_order_acquire);
    values[32] = atomic_load_explicit(&cpu->owner_wait.sequence, memory_order_acquire);
    PH_OK(ph_sched_records(index, now, labels + 2, values + 2,
        sizeof(values) / sizeof(*values) - 2, diagnostic_write));
}

static void *diagnostic_watchdog(void *unused) {
    (void)unused;
    const uint64_t rights = PACHA_FD_RIGHT_READ | PACHA_FD_RIGHT_WRITE |
        PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_CLOSE;
    long timer = pacha_syscall6(PACHA_FD_SYSCALL_TIMERFD_CREATE,
        PACHA_TIMERFD_CLOCK_MONOTONIC, 0, 0, 0, rights, 0);
    PH_CHECK(timer >= 16);
    uint64_t spec[4] = {2, 0, 2, 0};
    PH_OK(pacha_syscall4(PACHA_FD_SYSCALL_TIMERFD_SETTIME,
        timer, 0, (uintptr_t)spec, 0));
    static const char enabled[] = "[PH_SCHED] diagnostic watchdog enabled (native domain only)\n";
    PH_OK(diagnostic_write(enabled, sizeof(enabled) - 1));
    for (;;) {
        struct pacha_pollfd event = {.fd = (pacha_fd_t)timer, .events = PACHA_FD_EVENT_READABLE};
        PH_CHECK(ph_wait_readable(&event, 1) == 1);
        uint64_t expirations, now;
        PH_CHECK(pacha_syscall3(PACHA_FD_SYSCALL_READ, timer,
            (uintptr_t)&expirations, sizeof(expirations)) == sizeof(expirations));
        PH_OK(monotonic(&now));
        PH_CHECK(ph_current_cpu() < 0);
        for (unsigned i = 0; i < PH_CPU_COUNT; ++i) diagnostic_snapshot(i, now);
    }
}
#endif

void ph_machine_init(struct ph_image *image) {
    const uint64_t rights = PACHA_FD_RIGHT_READ | PACHA_FD_RIGHT_WRITE |
        PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_CLOSE;

    linux_notification_dispatch = image->core.dispatch;
    clock_page_map();
    for (unsigned i = 0; i < PH_CPU_COUNT; ++i) {
        struct ph_cpu *cpu = &cpus[i];

        PH_OK(kobox_machine_domain_init(&cpu->domain));
#if defined(PH_SCHED_DIAGNOSTIC) && PH_SCHED_DIAGNOSTIC
        atomic_init(&cpu->diagnostic_timer_arms, 0);
        atomic_init(&cpu->diagnostic_timer_expiries, 0);
        atomic_init(&cpu->diagnostic_notifies, 0);
        atomic_init(&cpu->diagnostic_notify_returns, 0);
        atomic_init(&cpu->diagnostic_callbacks, 0);
        atomic_init(&cpu->diagnostic_callback_returns, 0);
#endif
        cpu->timer_fd = (pacha_fd_t)pacha_syscall6(PACHA_FD_SYSCALL_TIMERFD_CREATE,
            PACHA_TIMERFD_CLOCK_MONOTONIC, 0, 0, 0, rights, 0);
        cpu->stop_fd = (pacha_fd_t)pacha_syscall3(PACHA_FD_SYSCALL_EVENTFD_CREATE, 0, rights, 0);
        PH_CHECK(cpu->timer_fd >= 16 && cpu->stop_fd >= 16);
        cpu->timer_task = ph_thread_create(timer_entry, (void *)(uintptr_t)i);
    }
#if defined(PH_SCHED_DIAGNOSTIC) && PH_SCHED_DIAGNOSTIC
    /* Only the diagnostic process owns this observer; process exit retires
     * its native timer/thread. It never acquires a logical CPU or calls Linux. */
    (void)ph_thread_create(diagnostic_watchdog, NULL);
#endif
}

const struct kobox_linux_task_host_operations ph_task_ops = {
    .size = sizeof(ph_task_ops),
    .identity = KOBOX_LINUX_TASK_HOST_IDENTITY,
    .task_bind_current = task_bind,
    .task_on_stack = task_on_stack,
    .task_create = task_create,
    .task_wake = task_wake,
    .task_park = task_park,
    .task_join_destroy = task_join,
    .task_destroy_current = task_destroy_current,
    .task_exit = ph_thread_exit,
    .cpu_enter = cpu_enter,
    .cpu_leave = cpu_leave,
    .cpu_switch = cpu_switch,
    .cpu_wait = cpu_wait,
    .cpu_stop = cpu_stop,
    .cpu_notify = cpu_notify,
    .cpu_irq_disable = irq_disable,
    .cpu_irq_enable = irq_enable,
    .notifications_save = ph_notifications_save,
    .notifications_restore = ph_notifications_restore,
    .cpu_irq_disabled = irq_disabled,
    .cpu_irq_bind = irq_bind,
    .cpu_irq_deliver = irq_deliver,
    .cpu_notification_sequence = notification_sequence,
    .monotonic_ns = monotonic,
    .realtime_ns = realtime,
    .clockevent_arm = clockevent_arm,
    .clockevent_cancel = clockevent_cancel,
    .clockevent_stop = clockevent_stop,
};
