/* SPDX-License-Identifier: MIT */
/* Translate native fault frames; Linux retains exception/fixup policy. */
#include "host.h"

static kobox_linux_exception_fn linux_exception_dispatch;

int ph_exceptions_install(kobox_linux_exception_fn dispatch) {
    PH_CHECK(dispatch != NULL && linux_exception_dispatch == NULL);
    linux_exception_dispatch = dispatch;
    return 0;
}

/* Linux exception handling may enable IRQs and preempt (local_irq_restore,
 * WARN continuation, irqentry_exit_cond_resched), i.e. switch fibers. The
 * native fault frame and fault stack are per thread, and the kernel delivers
 * no signal to the thread until that frame is returned. So the fault handler
 * only moves the fault onto the fiber's own stack: it builds this record
 * below the red zone, returns the fault into ph_fault_stub, and Linux runs
 * there. A synthesized ordinary signal frame then restores the full context;
 * ordinary returns carry no per-thread state, so they work after migration. */
struct ph_fault_record {
    struct pacha_native_signal_frame frame; /* First: 64-byte aligned xstate. */
    uint64_t vector, error_code, address;
    uint64_t mask;                          /* Notification mask at the fault. */
};
_Static_assert(sizeof(struct pacha_native_signal_frame) == PACHA_PROCESS_SIGNAL_FRAME_SIZE,
    "ordinary frame size");
_Static_assert(sizeof(struct pacha_native_fault_frame) == PACHA_PROCESS_FAULT_FRAME_SIZE,
    "fault frame size");

/* Admission only: the room a fault must leave below its record. It does not
 * bound use; nested signals in the stub each take the red zone, a frame and
 * the kernel's runtime area, and a stub that still reaches the guard page
 * faults outside these limits and becomes fatal there. */
enum { FAULT_WORK_BYTES = 64 * 1024, RED_ZONE_BYTES = 128 };

extern void ph_fault_stub(void);

static _Noreturn void fault_fatal(const struct pacha_native_fault_frame *native) {
    const struct pacha_native_signal_frame *saved = &native->signal;

    /* Publish the primary fault before best-effort stack inspection can
     * itself fault, and before process exit closes the service channel. */
    ph_report_exception(native);
    ph_number("fault vector", native->vector);
    ph_number("fault ip", saved->rip);
    ph_number("core offset", saved->rip - (uintptr_t)ph_core.base);
    ph_number("fault address", native->address);
    ph_number("fault logical cpu", (uint64_t)(int64_t)ph_current_cpu());
    ph_number("fault stack", saved->rsp);
    ph_number("fault rbp", saved->rbp);
    /* The native exception stack hides Linux's interrupted call chain.
     * Report only code pointers from the mapped stack page, never raw
     * stack data, when the Linux exception bridge cannot resume it. */
    if (saved->rsp >= 4096 && !(saved->rsp & 7)) {
        const uintptr_t core_start = (uintptr_t)ph_core.base;
        const size_t words = (4096 - (saved->rsp & 4095)) / sizeof(uintptr_t);
        const uintptr_t *stack = (const uintptr_t *)(uintptr_t)saved->rsp;
        unsigned shown = 0;
        for (size_t i = 0; i < words && shown < 8; ++i) {
            for (unsigned segment = 0; segment < ph_core.fixed.segment_count; ++segment) {
                const struct kobox_fixed_image_segment *code =
                    &ph_core.fixed.segments[segment];
                const uintptr_t begin = core_start + code->image_offset;
                if ((code->protection & KOBOX_FIXED_IMAGE_EXECUTE) &&
                    stack[i] >= begin && stack[i] - begin < code->mapping_size) {
                    ph_number("core stack candidate", stack[i] - core_start);
                    ++shown;
                    break;
                }
            }
        }
    }
    ph_fail(__FILE__, __LINE__, native->error_code);
}

/* On the native fault stack: no Linux, no fiber switch. */
void ph_exception(struct pacha_native_fault_frame *native) {
    struct pacha_native_signal_frame *saved = &native->signal;
    /* Masked or not, a pending notification waits for this frame's return. */
    struct ph_fiber *fiber = ph_current_fiber();
    uintptr_t sp = saved->rsp;

    /* Only a fault inside a Linux fiber's stack, with room for the record and
     * Linux's handler, moves there. Anything else (helpers, boot before core
     * entry, the guard page, an exhausted stack) stays fatal here: writing a
     * record outside such a stack would fault again, which the kernel
     * cannot deliver and would end the process without a report. */
    if (linux_exception_dispatch == NULL || fiber == NULL || fiber->logical_cpu < 0 ||
        sp - (uintptr_t)fiber->stack > fiber->stack_size ||
        sp - (uintptr_t)fiber->stack < RED_ZONE_BYTES + sizeof(struct ph_fault_record) +
            FAULT_WORK_BYTES + 64)
        fault_fatal(native);

    struct ph_fault_record *record = (struct ph_fault_record *)
        ((sp - RED_ZONE_BYTES - sizeof(*record)) & ~(uintptr_t)63);
    record->frame = *saved;
    record->frame.size = PACHA_PROCESS_SIGNAL_FRAME_SIZE;
    /* The kernel checks only the range; the number has no meaning on return. */
    record->frame.signo = PH_NOTIFICATION_SIGNAL;
    record->vector = native->vector;
    record->error_code = native->error_code;
    record->address = native->address;
    record->mask = PH_TLS_LOAD(uint64_t, notification_mask);

    /* The kernel completes a fault only for the frame it delivered, so this
     * frame is returned in place, redirected to the stub. The stub's stack
     * starts below the record, so its signal frames never overlap it. */
    saved->rip = (uintptr_t)ph_fault_stub;
    saved->rsp = (uintptr_t)record;
    saved->rdi = (uintptr_t)record;
    /* Notifications pending during the fault are delivered right after this
     * return, before the stub runs. Masked, they only leave a hint. Linux
     * must see the exception first: an int3 handled after a queued
     * text-poke IPI would no longer find its breakpoint and become fatal. */
    PH_TLS_STORE(notification_mask, (uint64_t)PH_NOTIFICATION_MASK);
}

/* On the fiber stack, masked, after the native fault was completed. */
_Noreturn void ph_fault_resume(struct ph_fault_record *record) {
    struct pacha_native_signal_frame *saved = &record->frame;
    struct kobox_linux_exception_frame exception = {
        .size = sizeof(exception),
        .ip = saved->rip,
        .sp = saved->rsp,
        .flags = saved->rflags,
        .fault_address = record->address,
        .error_code = record->error_code,
        .vector = (uint32_t)record->vector,
        .cpu = (uint32_t)ph_current_cpu(),
        .registers = {
            saved->rax, saved->rbx, saved->rcx, saved->rdx,
            saved->rsi, saved->rdi, saved->rbp,
            saved->r8, saved->r9, saved->r10, saved->r11,
            saved->r12, saved->r13, saved->r14, saved->r15,
        },
    };

    /* Linux may switch fibers in here; this continues on whichever thread
     * resumes it, which is why the mask came from the record. */
    if (linux_exception_dispatch(&exception) != KOBOX_EXCEPTION_RESUME) {
        struct pacha_native_fault_frame native = {
            .signal = *saved,
            .vector = record->vector,
            .error_code = record->error_code,
            .address = record->address,
        };
        native.signal.size = PACHA_PROCESS_FAULT_FRAME_SIZE;
        native.signal.signo = 0;
        fault_fatal(&native);
    }

    /* Linux exception-table fixups may change both the PC and result
     * registers; the native fault frame is gone, so they go into the frame
     * that restores the context. The kernel normalizes RFLAGS on return. */
    uint64_t *registers[] = {
        &saved->rax, &saved->rbx, &saved->rcx, &saved->rdx,
        &saved->rsi, &saved->rdi, &saved->rbp,
        &saved->r8, &saved->r9, &saved->r10, &saved->r11,
        &saved->r12, &saved->r13, &saved->r14, &saved->r15,
    };
    for (unsigned i = 0; i < KOBOX_EXCEPTION_REGISTERS; ++i) {
        *registers[i] = exception.registers[i];
    }
    saved->rip = exception.ip;
    saved->rsp = exception.sp;
    saved->rflags = exception.flags;
    PH_OK(ph_notifications_restore(record->mask));
    pacha_syscall2(PACHA_PROCESS_SYSCALL_SIGNAL_CTL, PACHA_PROCESS_SIGNAL_CTL_RETURN,
        (uintptr_t)saved);
    ph_fail(__FILE__, __LINE__, 0);
}
