/* SPDX-License-Identifier: MIT */
#include "host.h"
#include "arch/x86_64/fp_entry.h"
#if defined(PH_SCHED_DIAGNOSTIC) && PH_SCHED_DIAGNOSTIC
#include "scheduler_diagnostic.h"
_Static_assert(PH_THREAD_STACK_SIZE == 256 * 1024,
    "diagnostic bounds must match adapter-created and boot stacks");
_Static_assert(sizeof(((struct ph_task *)0)->diagnostic_stack_words) /
    sizeof(atomic_ullong) == PH_SCHED_STACK_WORDS, "diagnostic stack word count");
#endif

extern void ph_notification_entry(void);
extern void ph_thread_entry(void);
extern void ph_fiber_start(void);
extern void ph_fiber_thread_entry(void);
extern char __start_ph_trampoline[];
extern char __stop_ph_trampoline[];

KOBOX_GPR_LEAF int ph_notifications_save(uint64_t *previous_mask) {
    /* Only this native thread writes its mask. An upcall between load/store
     * either sees the old masked state or returns with that old mask restored;
     * it cannot concurrently retain a new mask while this thread resumes.
     * It may resume on another thread only if the mask was zero, and resumes
     * there unmasked too, so the loaded value stays right for whichever thread
     * the store reaches. That is why both accesses are %fs-relative and no
     * locked exchange is needed on every entry. Deferred hints have remote
     * writers and retain their RMW. The asm memory clobbers order protected
     * work after the store, as a signal fence would.
     */
    uint64_t previous = PH_TLS_LOAD(uint64_t, notification_mask);
    PH_TLS_STORE(notification_mask, (uint64_t)PH_NOTIFICATION_MASK);
    *previous_mask = previous;
    return 0;
}

static __attribute__((noinline, used)) int ph_notifications_dispatch_body(void) {
    ph_dispatch_notifications();
    return 0;
}

__attribute__((naked, noinline)) int ph_notifications_dispatch_preserved(void) {
    KOBOX_FP_ENTRY_BODY(ph_notifications_dispatch_body);
}

KOBOX_GPR_LEAF int ph_notifications_restore(uint64_t mask) {
    PH_CHECK(mask == 0 || mask == PH_NOTIFICATION_MASK);
    PH_TLS_STORE(notification_mask, mask);
    /* After an unmasking store this code may already run on another thread;
     * the exchange then takes that thread's hint, which this unmasked fiber
     * may dispatch. The hint it left behind stays with its own thread. */
    if (!mask && PH_TLS_EXCHANGE(notification_deferred, 0u))
        return ph_notifications_dispatch_preserved();
    return 0;
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
    /* Native delivery remains asynchronous. A protected host operation only
     * defers Linux entry; the CPU domain retains every event and its count.
     * Unmasking drains this hint before returning to the interrupted caller.
     * Linux may switch fibers inside the dispatch below; the frame lives on
     * this fiber's stack and returns statelessly on whichever thread resumes
     * it, so nothing after dispatch may use this thread. */
#if defined(PH_SCHED_DIAGNOSTIC) && PH_SCHED_DIAGNOSTIC
    /* Diagnostic snapshot only: a nested unmasked upcall can migrate this
     * handler, which at worst records into the thread it started on. */
    struct ph_task *task = ph_current_task();
    struct ph_fiber *fiber = ph_current_fiber();
    unsigned frame_free = 0;
    /* A nested upcall must not tear the outer frame snapshot. Skip it rather
     * than wait in the signal handler; readers explicitly check validity. */
    if (atomic_compare_exchange_strong_explicit(&task->diagnostic_frame_busy,
            &frame_free, 1, memory_order_acquire, memory_order_relaxed)) {
        atomic_fetch_add_explicit(&task->diagnostic_frame_sequence, 1,
            memory_order_acq_rel);
        atomic_store_explicit(&task->diagnostic_rip, frame->rip, memory_order_relaxed);
        atomic_store_explicit(&task->diagnostic_rsp, frame->rsp, memory_order_relaxed);
        atomic_store_explicit(&task->diagnostic_rdi, frame->rdi, memory_order_relaxed);
        atomic_store_explicit(&task->diagnostic_rsi, frame->rsi, memory_order_relaxed);
        atomic_store_explicit(&task->diagnostic_rax, frame->rax, memory_order_relaxed);
        atomic_store_explicit(&task->diagnostic_rbx, frame->rbx, memory_order_relaxed);
        uint64_t words[PH_SCHED_STACK_WORDS];
        int stack_valid = ph_sched_stack_words(frame->rsp,
            fiber ? fiber->stack : task->stack, task->fault_stack,
            PH_THREAD_STACK_SIZE, words);
        atomic_store_explicit(&task->diagnostic_stack_valid, stack_valid, memory_order_relaxed);
        for (unsigned i = 0; i < PH_SCHED_STACK_WORDS; ++i)
            atomic_store_explicit(&task->diagnostic_stack_words[i], words[i], memory_order_relaxed);
        atomic_store_explicit(&task->diagnostic_frame_mask,
            PH_TLS_LOAD(uint64_t, notification_mask), memory_order_relaxed);
        atomic_fetch_add_explicit(&task->diagnostic_frame_sequence, 1,
            memory_order_release);
        atomic_store_explicit(&task->diagnostic_frame_busy, 0, memory_order_release);
    }
#endif
    /* A set mask also rules out migration between these two accesses. */
    if (PH_TLS_LOAD(uint64_t, notification_mask)) {
        PH_TLS_STORE(notification_deferred, 1u);
        return;
    }
    ph_dispatch_notifications();
}

void ph_thread_setup(struct ph_task *task) {
    atomic_init(&task->notification_mask, PH_NOTIFICATION_MASK);
    atomic_init(&task->notification_deferred, 0);
#if defined(PH_SCHED_DIAGNOSTIC) && PH_SCHED_DIAGNOSTIC
    atomic_init(&task->diagnostic_frame_busy, 0);
    atomic_init(&task->diagnostic_frame_sequence, 0);
    atomic_init(&task->diagnostic_rip, 0);
    atomic_init(&task->diagnostic_rsp, 0);
    atomic_init(&task->diagnostic_rdi, 0);
    atomic_init(&task->diagnostic_rsi, 0);
    atomic_init(&task->diagnostic_rax, 0);
    atomic_init(&task->diagnostic_rbx, 0);
    atomic_init(&task->diagnostic_stack_valid, 0);
    for (unsigned i = 0; i < PH_SCHED_STACK_WORDS; ++i)
        atomic_init(&task->diagnostic_stack_words[i], 0);
    atomic_init(&task->diagnostic_frame_mask, 0);
    atomic_init(&task->diagnostic_wait_word, 0);
    atomic_init(&task->diagnostic_wait_expected, 0);
    PH_CHECK(atomic_is_lock_free(&task->diagnostic_frame_sequence) &&
        atomic_is_lock_free(&task->diagnostic_wait_word));
#endif
    PH_CHECK(atomic_is_lock_free(&task->notification_mask) &&
        atomic_is_lock_free(&task->notification_deferred));
    PH_OK(pacha_syscall6(PACHA_PROCESS_SYSCALL_SIGNAL_CTL,
        PACHA_PROCESS_SIGNAL_CTL_REGISTER, (uintptr_t)ph_notification_entry,
        (uintptr_t)__start_ph_trampoline, (uintptr_t)__stop_ph_trampoline, 0, 0));
    PH_OK(pacha_syscall4(PACHA_PROCESS_SYSCALL_SIGNAL_CTL,
        PACHA_PROCESS_SIGNAL_CTL_REGISTER_FAULT, (uintptr_t)ph_notification_entry,
        (uintptr_t)task->fault_stack, PH_THREAD_STACK_SIZE));
    /* New threads can inherit a blocked native mask from their creator. */
    PH_OK(pacha_syscall2(PACHA_PROCESS_SYSCALL_SIGNAL_CTL,
        PACHA_PROCESS_SIGNAL_CTL_SET_MASK, 0));
}

static struct ph_task *thread_alloc(void) {
    struct ph_task *task = ph_alloc(sizeof(*task));

    task->self = task;
    task->notification_mask = PH_NOTIFICATION_MASK;
    task->fault_stack = ph_alloc(PH_THREAD_STACK_SIZE);
    return task;
}

static void thread_start(struct ph_task *task, void (*entry)(void), uintptr_t stack_top) {
    const uint64_t rights = PACHA_FD_RIGHT_START | PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_PROCESS_SIGNAL;
    long thread_fd = pacha_syscall6(PACHA_THREAD_SYSCALL_CREATE, PACHA_PROCESS_SELF_FD,
        (uintptr_t)entry, stack_top, 0, (uintptr_t)task, rights);
    PH_CHECK(thread_fd >= 16);

    /* Publish the handle before START: the new thread can run immediately. */
    task->fd = (int)thread_fd;
    PH_OK(pacha_syscall1(PACHA_THREAD_SYSCALL_START, thread_fd));
}

struct ph_task *ph_thread_create(void *(*entry)(void *), void *argument) {
    struct ph_task *task = thread_alloc();

    task->entry = entry;
    task->argument = argument;
    task->stack = ph_alloc(PH_THREAD_STACK_SIZE);
    thread_start(task, ph_thread_entry, (uintptr_t)task->stack + PH_THREAD_STACK_SIZE);
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

    if (task->stack)
        ph_free(task->stack, PH_THREAD_STACK_SIZE);
    ph_free(task->fault_stack, PH_THREAD_STACK_SIZE);
    ph_free(task, sizeof(*task));
    return 0;
}

/* Initial frame consumed by ph_fiber_resume_frame: MXCSR and x87 control word
 * in the first word, then r15..r12, rbp, rbx (= fiber), return address. */
enum { FIBER_FRAME_WORDS = 8 };

struct ph_fiber *ph_fiber_create(void *(*entry)(void *), void *argument) {
    struct ph_fiber *fiber = ph_alloc(sizeof(*fiber));

    /* The lowest page stays inaccessible: an overflow faults instead of
     * silently writing the neighbouring mapping. */
    fiber->mapping = ph_alloc(PH_PAGE_SIZE + PH_THREAD_STACK_SIZE);
    PH_OK(pacha_syscall3(PACHA_VM_SYSCALL_MPROTECT, (uintptr_t)fiber->mapping,
        PH_PAGE_SIZE, 0));
    fiber->stack = (char *)fiber->mapping + PH_PAGE_SIZE;
    fiber->stack_size = PH_THREAD_STACK_SIZE;
    fiber->entry = entry;
    fiber->argument = argument;
    fiber->state = PH_FIBER_NEW;
    fiber->logical_cpu = -1;
    uintptr_t top = (uintptr_t)fiber->stack + fiber->stack_size;
    uint64_t *frame = (uint64_t *)((top - FIBER_FRAME_WORDS * sizeof(uint64_t)) & ~(uintptr_t)15);
    frame[0] = UINT64_C(0x1f80) | (UINT64_C(0x037f) << 32);
    for (unsigned i = 1; i < 6; ++i) frame[i] = 0;
    frame[6] = (uintptr_t)fiber;
    frame[7] = (uintptr_t)ph_fiber_start;
    fiber->rsp = (uintptr_t)frame;
    return fiber;
}

void ph_fiber_destroy(struct ph_fiber *fiber) {
    PH_CHECK(!fiber->bound);
    /* An origin's thread is still on this stack when Linux releases it:
     * right after an abort wake, or while the idle task leaves its CPU.
     * Only its exit makes the fiber DEAD and the stack idle. */
    if (fiber->origin) {
        PH_OK(ph_thread_join(fiber->origin));
        PH_CHECK(fiber->state == PH_FIBER_DEAD);
    } else {
        PH_CHECK(fiber->state == PH_FIBER_NEW || fiber->state == PH_FIBER_DEAD);
    }
    ph_free(fiber->mapping, PH_PAGE_SIZE + PH_THREAD_STACK_SIZE);
    ph_free(fiber, sizeof(*fiber));
}

struct ph_task *ph_fiber_thread_start(struct ph_fiber *fiber) {
    PH_CHECK(fiber->state == PH_FIBER_NEW && !fiber->origin);
    struct ph_task *task = thread_alloc();

    task->fiber = fiber;
    fiber->origin = task;
    fiber->state = PH_FIBER_RUNNING;
    thread_start(task, ph_fiber_thread_entry, fiber->rsp);
    return task;
}

/* Runs below the initial frame before ph_fiber_thread_entry resumes it. */
void ph_fiber_thread_setup(void) {
    ph_thread_setup(ph_current_task());
}

_Noreturn void ph_fiber_main(struct ph_fiber *fiber) {
    PH_CHECK(fiber == ph_current_fiber() && fiber->state == PH_FIBER_RUNNING);
    (void)fiber->entry(fiber->argument);
    /* Only an origin may return: Linux tasks otherwise leave through a final
     * exiting switch. The thread exits on the stack its joiner releases.
     * Both returns leave notifications masked (an aborted start never
     * unmasked; cpu_leave restores the entry mask), so this thread read
     * cannot migrate. */
    PH_CHECK(fiber->origin == ph_current_task());
    fiber->state = PH_FIBER_DEAD;
    ph_thread_exit();
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
