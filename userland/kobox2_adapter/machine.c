/* SPDX-License-Identifier: MIT */
#include "host.h"

#define NANOSECONDS_PER_SECOND UINT64_C(1000000000)
#define CPU_STOP_TIMEOUT_NS (2 * NANOSECONDS_PER_SECOND)

struct ph_cpu {
    struct kobox_machine_domain domain;
    atomic_uint owner_lock;
    atomic_uint owner_sequence;

    /* Timer programming and expiry publication use a separate lock.
     * Lock order is timer_lock -> owner_lock, never the reverse. */
    atomic_uint timer_lock;
    int timer_fd;
    int stop_fd;
    uint64_t deadline_ns;
    struct ph_task *timer_task;
};

static struct ph_cpu cpus[PH_CPU_COUNT];
static kobox_linux_task_notification_fn linux_notification_dispatch;

static int monotonic(uint64_t *nanoseconds);

/* All callers hold owner_lock and mask native notifications before taking it.
 * Waiters snapshot the sequence while locked, then futex-wait after unlocking. */
static void wake_cpu_waiters(struct ph_cpu *cpu) {
    atomic_fetch_add_explicit(&cpu->owner_sequence, 1, memory_order_release);
    ph_wake(&cpu->owner_sequence);
}

static int task_wake(void *opaque) {
    struct ph_task *task = opaque;
    unsigned previous = atomic_fetch_add_explicit(&task->permit, 1, memory_order_release);

    PH_CHECK(previous != UINT32_MAX);
    ph_wake(&task->permit);
    return 0;
}

static int task_park(void *opaque) {
    struct ph_task *task = opaque;

    PH_CHECK(task == ph_current_task());
    for (;;) {
        unsigned permits = atomic_load_explicit(&task->permit, memory_order_acquire);

        if (permits != 0 && atomic_compare_exchange_weak_explicit(
                &task->permit, &permits, permits - 1,
                memory_order_acquire, memory_order_relaxed)) {
            return 0;
        }
        if (permits == 0) {
            ph_wait(&task->permit, 0);
        }
    }
}

struct task_start {
    void *(*entry)(void *);
    void *argument;
};

static void *guest_entry(void *opaque) {
    struct task_start start = *(struct task_start *)opaque;

    ph_free(opaque, sizeof(start));
    /* A newly created native thread cannot run Linux until Linux wakes it. */
    PH_OK(task_park(ph_current_task()));
    return start.entry(start.argument);
}

static int task_create(void **out, void *(*entry)(void *), void *argument) {
    uint64_t previous_mask;

    PH_OK(ph_notifications_save(&previous_mask));
    struct task_start *start = ph_alloc(sizeof(*start));

    *start = (struct task_start) {
        .entry = entry,
        .argument = argument,
    };
    *out = ph_thread_create(guest_entry, start);
    PH_OK(ph_notifications_restore(previous_mask));
    return 0;
}

static int task_bind(void **out) {
    *out = ph_current_task();
    return 0;
}

static int task_destroy_current(void *task) {
    PH_CHECK(task == ph_current_task() && ph_current_task()->bound);
    return 0;
}

static int task_join(void *task) {
    uint64_t previous_mask;

    PH_OK(ph_notifications_save(&previous_mask));
    int result = ph_thread_join(task);
    PH_OK(ph_notifications_restore(previous_mask));
    return result;
}

static int cpu_enter(uint32_t index, void *opaque) {
    struct ph_task *task = ph_current_task();

    PH_CHECK(index < PH_CPU_COUNT && task == opaque && task->logical_cpu < 0);
    struct ph_cpu *cpu = &cpus[index];

    PH_OK(ph_notifications_save(&task->saved_notification_mask));
    ph_lock(&cpu->owner_lock);
    while (!kobox_machine_domain_can_enter(&cpu->domain, task)) {
        unsigned observed = atomic_load_explicit(
            &cpu->owner_sequence, memory_order_acquire);

        ph_unlock(&cpu->owner_lock);
        ph_wait(&cpu->owner_sequence, observed);
        ph_lock(&cpu->owner_lock);
    }
    PH_OK(kobox_machine_domain_enter(&cpu->domain, task));
    task->logical_cpu = (int)index;
    ph_unlock(&cpu->owner_lock);

    /* Drain notifications published while the domain had no running owner. */
    PH_OK(ph_notifications_restore(0));
    ph_dispatch_notifications();
    return 0;
}

static int cpu_leave(uint32_t index) {
    struct ph_task *task = ph_current_task();
    uint64_t previous_mask;

    PH_CHECK(index < PH_CPU_COUNT && task->logical_cpu == (int)index);
    struct ph_cpu *cpu = &cpus[index];

    PH_OK(ph_notifications_save(&previous_mask));
    PH_CHECK(atomic_load(&cpu->domain.irq_depth) == 0);
    ph_lock(&cpu->owner_lock);
    kobox_machine_domain_release(&cpu->domain);
    task->logical_cpu = -1;
    wake_cpu_waiters(cpu);
    ph_unlock(&cpu->owner_lock);
    PH_OK(ph_notifications_restore(task->saved_notification_mask));
    return 0;
}

static int cpu_switch(uint32_t index, void *previous, void *next, uint8_t exiting) {
    struct ph_task *task = ph_current_task();
    uint64_t previous_mask;

    PH_CHECK(index < PH_CPU_COUNT && task == previous && task->logical_cpu == (int)index);
    struct ph_cpu *cpu = &cpus[index];

    PH_OK(ph_notifications_save(&previous_mask));
    ph_lock(&cpu->owner_lock);
    PH_CHECK(atomic_load(&task->permit) == 0);

    /* Publish the successor before waking it. Its cpu_enter still waits for
     * handoff_finish, so both threads can never execute in this domain. */
    PH_OK(kobox_machine_domain_handoff(&cpu->domain, previous, next));
    PH_OK(task_wake(next));
    task->logical_cpu = -1;
    kobox_machine_domain_handoff_finish(&cpu->domain);
    wake_cpu_waiters(cpu);
    ph_unlock(&cpu->owner_lock);

    PH_OK(ph_notifications_restore(task->saved_notification_mask));
    if (exiting) {
        ph_thread_exit();
    }
    return task_park(previous);
}

static int cpu_wait(uint32_t index, uint64_t observed, uint64_t *sequence_out) {
    uint64_t previous_mask;

    PH_CHECK(index < PH_CPU_COUNT && ph_current_task()->logical_cpu == (int)index);
    struct ph_cpu *cpu = &cpus[index];

    PH_OK(ph_notifications_save(&previous_mask));
    ph_lock(&cpu->owner_lock);
    while (kobox_machine_domain_should_wait(&cpu->domain, observed)) {
        unsigned owner_sequence = atomic_load_explicit(
            &cpu->owner_sequence, memory_order_acquire);

        ph_unlock(&cpu->owner_lock);
        ph_wait(&cpu->owner_sequence, owner_sequence);
        ph_lock(&cpu->owner_lock);
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

    ph_lock(&cpu->owner_lock);
    PH_OK(kobox_machine_domain_notify(&cpu->domain, notification_kind(kind)));
    struct ph_task *owner = cpu->domain.owner;

    wake_cpu_waiters(cpu);
    /* Keep the owner locked through SIGNAL: it must not exit and lose its FD
     * between lookup and delivery. The domain retains the actual event count. */
    if (owner != NULL) {
        PH_OK(pacha_syscall2(PACHA_THREAD_SYSCALL_SIGNAL, owner->fd, PH_NOTIFICATION_SIGNAL));
    }
    ph_unlock(&cpu->owner_lock);
    PH_OK(ph_notifications_restore(previous_mask));
    return 0;
}

static _Noreturn void acknowledge_cpu_stop(struct ph_cpu *cpu) {
    atomic_store_explicit(&cpu->domain.stopped, true, memory_order_release);
    for (;;) {
        struct ph_task *task = ph_current_task();

        ph_wait(&task->permit, atomic_load(&task->permit));
    }
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
        int index = ph_current_task()->logical_cpu;

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
        linux_notification_dispatch((uint32_t)index, linux_kinds[kind], count);

        /* Callback execution can migrate this task. IRQ return belongs to
         * the current domain, not necessarily the domain used for entry. */
        uint64_t callback_mask;
        PH_OK(ph_notifications_save(&callback_mask));
        index = ph_current_task()->logical_cpu;
        PH_CHECK(index >= 0 && kobox_machine_domain_irq_return(&cpus[index].domain));
    }
    PH_OK(ph_notifications_restore(previous_mask));
}

static int irq_disable(uint32_t index) {
    PH_CHECK(index < PH_CPU_COUNT && ph_current_task()->logical_cpu == (int)index);
    return (int)kobox_machine_domain_irq_disable(&cpus[index].domain);
}

static int irq_enable(uint32_t index) {
    bool dispatch_pending;

    PH_CHECK(index < PH_CPU_COUNT && ph_current_task()->logical_cpu == (int)index);
    PH_OK(kobox_machine_domain_irq_enable(&cpus[index].domain, &dispatch_pending));
    if (dispatch_pending) {
        ph_dispatch_notifications();
    }
    return 0;
}

static uint8_t irq_disabled(uint32_t index) {
    PH_CHECK(index < PH_CPU_COUNT);
    return atomic_load(&cpus[index].domain.irq_depth) != 0;
}

static uint64_t notification_sequence(uint32_t index) {
    PH_CHECK(index < PH_CPU_COUNT);
    return atomic_load(&cpus[index].domain.sequence);
}

static int clock_ns(unsigned clock_id, uint64_t *nanoseconds) {
    uint64_t seconds_and_nanoseconds[2];
    long result = pacha_syscall2(PACHA_RUNTIME_SYSCALL_CLOCK_GETTIME,
        clock_id, (uintptr_t)seconds_and_nanoseconds);

    if (result != 0) {
        return (int)result;
    }
    PH_CHECK(seconds_and_nanoseconds[0] <= UINT64_MAX / NANOSECONDS_PER_SECOND);
    *nanoseconds = seconds_and_nanoseconds[0] * NANOSECONDS_PER_SECOND +
        seconds_and_nanoseconds[1];
    return 0;
}

static int monotonic(uint64_t *nanoseconds) {
    return clock_ns(PACHA_TIMERFD_CLOCK_MONOTONIC, nanoseconds);
}

static int realtime(uint64_t *nanoseconds) {
    return clock_ns(0, nanoseconds);
}

static int cpu_stop(uint32_t index) {
    uint64_t started_at;
    uint64_t now;

    PH_CHECK(index < PH_CPU_COUNT && ph_current_task()->logical_cpu != (int)index);
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
    PH_OK(pacha_syscall4(PACHA_FD_SYSCALL_TIMERFD_SETTIME, cpu->timer_fd,
        PACHA_TIMERFD_ABSTIME, (uintptr_t)timer_spec, 0));
    ph_unlock(&cpu->timer_lock);
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
        PH_OK(cpu_notify(index, KOBOX_LINUX_TASK_CLOCKEVENT));
    }
    ph_unlock(&cpu->timer_lock);
}

static void *timer_entry(void *opaque) {
    uint32_t index = (uint32_t)(uintptr_t)opaque;
    struct ph_cpu *cpu = &cpus[index];

    for (;;) {
        struct pacha_pollfd fds[2] = {
            {.fd = cpu->timer_fd, .events = PACHA_FD_EVENT_READABLE},
            {.fd = cpu->stop_fd, .events = PACHA_FD_EVENT_READABLE},
        };
        long ready_count = pacha_syscall4(PACHA_FD_SYSCALL_WAIT_MANY,
            (uintptr_t)fds, 2, PACHA_FD_WAIT_FOREVER, 0);

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

void ph_machine_init(struct ph_image *image) {
    const uint64_t rights = PACHA_FD_RIGHT_READ | PACHA_FD_RIGHT_WRITE |
        PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_CLOSE;

    linux_notification_dispatch = image->core.dispatch;
    for (unsigned i = 0; i < PH_CPU_COUNT; ++i) {
        struct ph_cpu *cpu = &cpus[i];

        PH_OK(kobox_machine_domain_init(&cpu->domain));
        cpu->timer_fd = (int)pacha_syscall6(PACHA_FD_SYSCALL_TIMERFD_CREATE,
            PACHA_TIMERFD_CLOCK_MONOTONIC, 0, 0, 0, rights, 0);
        cpu->stop_fd = (int)pacha_syscall3(PACHA_FD_SYSCALL_EVENTFD_CREATE, 0, rights, 0);
        PH_CHECK(cpu->timer_fd >= 16 && cpu->stop_fd >= 16);
        cpu->timer_task = ph_thread_create(timer_entry, (void *)(uintptr_t)i);
    }
}

const struct kobox_linux_task_host_operations ph_task_ops = {
    .size = sizeof(ph_task_ops),
    .identity = KOBOX_LINUX_TASK_HOST_IDENTITY,
    .task_bind_current = task_bind,
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
    .cpu_notification_sequence = notification_sequence,
    .monotonic_ns = monotonic,
    .realtime_ns = realtime,
    .clockevent_arm = clockevent_arm,
    .clockevent_cancel = clockevent_cancel,
    .clockevent_stop = clockevent_stop,
};
