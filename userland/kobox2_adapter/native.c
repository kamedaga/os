/* SPDX-License-Identifier: MIT */
#include "host.h"

extern void ph_notification_entry(void);
extern void ph_thread_entry(void);
extern char __start_ph_trampoline[];
extern char __stop_ph_trampoline[];

int ph_notifications_save(uint64_t *previous_mask) {
    struct ph_task *task = ph_current_task();

    *previous_mask = task->notification_mask;
    task->notification_mask = PH_NOTIFICATION_MASK;
    return (int)pacha_syscall2(PACHA_PROCESS_SYSCALL_SIGNAL_CTL,
        PACHA_PROCESS_SIGNAL_CTL_SET_MASK, PH_NOTIFICATION_MASK);
}

int ph_notifications_restore(uint64_t mask) {
    PH_CHECK(mask == 0 || mask == PH_NOTIFICATION_MASK);
    ph_current_task()->notification_mask = mask;
    return (int)pacha_syscall2(PACHA_PROCESS_SYSCALL_SIGNAL_CTL,
        PACHA_PROCESS_SIGNAL_CTL_SET_MASK, mask);
}

/* Only the assembly prologue/return is inhibited. Linux may enable nested
 * notifications inside its callback and may return on another logical CPU. */
void ph_notification_body(struct pacha_native_signal_frame *frame) {
    PH_CHECK(frame->magic == PACHA_PROCESS_SIGNAL_FRAME_MAGIC);

    if (frame->signo == 0) {
        ph_exception((struct pacha_native_fault_frame *)frame);
        return;
    }

    PH_CHECK(frame->signo == PH_NOTIFICATION_SIGNAL);
    ph_dispatch_notifications();
}

void ph_thread_setup(struct ph_task *task) {
    PH_OK(pacha_syscall6(PACHA_PROCESS_SYSCALL_SIGNAL_CTL,
        PACHA_PROCESS_SIGNAL_CTL_REGISTER, (uintptr_t)ph_notification_entry,
        (uintptr_t)__start_ph_trampoline, (uintptr_t)__stop_ph_trampoline, 0, 0));
    PH_OK(pacha_syscall4(PACHA_PROCESS_SYSCALL_SIGNAL_CTL,
        PACHA_PROCESS_SIGNAL_CTL_REGISTER_FAULT, (uintptr_t)ph_notification_entry,
        (uintptr_t)task->fault_stack, PH_THREAD_STACK_SIZE));
    PH_OK(ph_notifications_restore(PH_NOTIFICATION_MASK));
}

struct ph_task *ph_thread_create(void *(*entry)(void *), void *argument) {
    const uint64_t rights = PACHA_FD_RIGHT_START | PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_PROCESS_SIGNAL;
    struct ph_task *task = ph_alloc(sizeof(*task));

    task->self = task;
    task->logical_cpu = -1;
    task->notification_mask = PH_NOTIFICATION_MASK;
    task->entry = entry;
    task->argument = argument;
    task->stack = ph_alloc(PH_THREAD_STACK_SIZE);
    task->fault_stack = ph_alloc(PH_THREAD_STACK_SIZE);
    task->tls_size = ph_core.tls_size;
    task->tls = ph_alloc(task->tls_size);
    memcpy(task->tls, ph_core.tls_template, ph_core.tls_filesz);

    long thread_fd = pacha_syscall6(PACHA_THREAD_SYSCALL_CREATE, PACHA_PROCESS_SELF_FD,
        (uintptr_t)ph_thread_entry, (uintptr_t)task->stack + PH_THREAD_STACK_SIZE, 0,
        (uintptr_t)task, rights);
    PH_CHECK(thread_fd >= 16);

    /* Publish the handle before START: the new thread can run immediately. */
    task->fd = (int)thread_fd;
    PH_OK(pacha_syscall1(PACHA_THREAD_SYSCALL_START, thread_fd));
    return task;
}

void ph_thread_body(void) {
    struct ph_task *task = ph_current_task();

    ph_thread_setup(task);
    (void)task->entry(task->argument);
    ph_thread_exit();
}

_Noreturn void ph_thread_exit(void) {
    PH_OK(pacha_syscall3(PACHA_THREAD_SYSCALL_EXIT, 0, 0, 0));
    ph_fail(__FILE__, __LINE__, 0);
}

int ph_thread_join(void *opaque) {
    struct ph_task *task = opaque;
    struct pacha_pollfd completion = {
        .fd = task->fd,
        .events = PACHA_FD_EVENT_READABLE,
    };
    uint64_t status[4];

    PH_CHECK(task != ph_current_task() && !task->bound);
    long ready_count = ph_wait_readable(&completion, 1);
    PH_CHECK(ready_count == 1 && completion.revents == PACHA_FD_EVENT_READABLE);

    /* THREAD_WAIT returns state and exit code in the first two words.
     * State 2 is exited; storage cannot be reclaimed before this point. */
    PH_OK(pacha_syscall2(PACHA_THREAD_SYSCALL_WAIT, task->fd, (uintptr_t)status));
    PH_CHECK(status[0] == 2 && status[1] == 0);
    PH_OK(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, task->fd));

    ph_free(task->stack, PH_THREAD_STACK_SIZE);
    ph_free(task->fault_stack, PH_THREAD_STACK_SIZE);
    ph_free(task->tls, task->tls_size);
    ph_free(task, sizeof(*task));
    return 0;
}

long ph_wait_readable(struct pacha_pollfd *descriptors, size_t count) {
    for (;;) {
        for (size_t index = 0; index < count; ++index) {
            descriptors[index].revents = 0;
        }
        long result = pacha_syscall4(PACHA_FD_SYSCALL_WAIT_MANY,
            (uintptr_t)descriptors, count, PACHA_FD_WAIT_FOREVER, 0);
        size_t ready = 0;

        for (size_t index = 0; index < count; ++index) {
            ready += descriptors[index].revents != 0;
        }
        if (result > 0 && (size_t)result == ready) {
            return result;
        }
        /* NOT_READY is an interrupted wait, not completion. Its positive
         * native value also equals a valid two-FD ready count. */
        if (result != PACHA_SYSCALL_ERR_NOT_READY || ready) {
            ph_fail(__FILE__, __LINE__, result);
        }
    }
}
