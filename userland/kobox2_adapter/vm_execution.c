/* SPDX-License-Identifier: MIT */
/* Native thread control. Linux supplies the user entry, stack and TLS image. */
#include "vm_internal.h"
#include <errno.h>

static long thread_context(struct ph_vm_space *space, unsigned operation,
    struct pacha_thread_context *context) {
    return pacha_syscall4(PACHA_THREAD_SYSCALL_CONTEXT, space->thread_fd,
        operation, (uintptr_t)context, sizeof(*context));
}

static int control_error(struct ph_vm_space *space, long result) {
    uint64_t status[4];
    long waited = pacha_syscall2(PACHA_PROCESS_SYSCALL_WAIT,
        space->process_fd, (uintptr_t)status);

    /* Native control returns INVALID for an already dead target too.
     * Confirm actual termination instead of guessing from that errno.
     * WAIT is a status read; monitor still owns the single EXIT event and
     * native resource draining still belongs to close_space. */
    if (!waited) {
        PH_CHECK(status[0] == 2 || status[0] == 3);
        return -ESRCH;
    }
    PH_CHECK(waited == PACHA_SYSCALL_ERR_NOT_READY);
    return ph_vm_native_error(result);
}

void ph_vm_park_bootstrap(struct ph_vm_space *space) {
    struct ph_vm_client_control *control = space->control;
    struct pacha_pollfd ready[2] = {
        {.fd = space->event_fd, .events = PACHA_FD_EVENT_READABLE},
        {.fd = space->process_fd, .events = PACHA_FD_EVENT_READABLE},
    };

    control->command = PH_VM_CLIENT_PARK;
    atomic_store_explicit(&control->command_sequence, ++space->bootstrap_sequence,
        memory_order_release);
    ph_vm_event_signal(space->kick_fd);
    PH_CHECK(ph_wait_readable(ready, 2) == 1);
    PH_CHECK(ready[0].revents == PACHA_FD_EVENT_READABLE && !ready[1].revents);
    ph_vm_event_consume(space->event_fd);
    PH_CHECK(atomic_load_explicit(&control->event_sequence, memory_order_acquire) ==
        space->bootstrap_sequence && control->event == PH_VM_CLIENT_PARKED);

    uint64_t started, now;

    PH_OK(ph_task_ops.monotonic_ns(&started));
    for (;;) {
        PH_OK(pacha_syscall2(PACHA_PROCESS_SYSCALL_STOP, space->process_fd, 19));
        long result = thread_context(space, PACHA_THREAD_CONTEXT_GET, &space->initial_context);

        if (!result) {
            break;
        }
        /* PARKED publication can be observed before its native write returns.
         * Never rewrite that active wait. Let it retire, then stop again. */
        PH_CHECK(result == PACHA_SYSCALL_ERR_NOT_READY);
        PH_OK(pacha_syscall2(PACHA_PROCESS_SYSCALL_CONTINUE, space->process_fd, 0));
        PH_OK(ph_task_ops.monotonic_ns(&now));
        PH_CHECK(now - started < UINT64_C(5000000000));
    }
    space->native_stopped = true;
    memcpy(&space->fp_mxcsr_mask, space->initial_context.xstate + 28,
        sizeof(space->fp_mxcsr_mask));
}

static void set_user_context(struct pacha_thread_context *native,
    const struct kobox_x86_user_regs *user, const struct kobox_x86_fp_state *fp) {
    /* Preserve native selectors and metadata obtained with GET. Linux's
     * selectors describe its hosted ABI, not the PachaOS GDT. */
    uint64_t cs = native->registers.cs, ss = native->registers.ss;

    native->registers = (struct pacha_thread_registers) {
        .r15 = user->r15, .r14 = user->r14, .r13 = user->r13, .r12 = user->r12,
        .r11 = user->r11, .r10 = user->r10, .r9 = user->r9, .r8 = user->r8,
        .rbp = user->bp, .rdi = user->di, .rsi = user->si, .rdx = user->dx,
        .rcx = user->cx, .rbx = user->bx, .rax = user->ax,
        .rip = user->ip, .rsp = user->sp, .rflags = user->flags, .cs = cs, .ss = ss,
    };
    native->fs_base = user->fs_base;
    native->gs_base = user->gs_base;
    memcpy(native->xstate, fp->fxsave, sizeof(fp->fxsave));
    native->xstate[512] |= 3; /* The supplied x87/SSE bank is present. */
}

static void capture_user_context(struct ph_vm_space *space,
    const struct pacha_thread_context *context) {
    const struct pacha_thread_registers *in = &context->registers;
    struct ph_vm_client_registers user = {
        .rax = in->rax, .rbx = in->rbx, .rcx = in->rcx, .rdx = in->rdx,
        .rsi = in->rsi, .rdi = in->rdi, .rbp = in->rbp,
        .r8 = in->r8, .r9 = in->r9, .r10 = in->r10, .r11 = in->r11,
        .r12 = in->r12, .r13 = in->r13, .r14 = in->r14, .r15 = in->r15,
        .rip = in->rip, .rsp = in->rsp, .rflags = in->rflags,
        .fs_base = context->fs_base, .gs_base = context->gs_base,
    };

    /* Keep the whole XSAVE image for the next SET; Linux replaces only
     * x87/SSE, not an interrupted AVX bank or native context metadata. */
    space->initial_context = *context;
    space->captured_registers = ph_vm_import_registers(&user);
    space->captured_registers.orig_ax = UINT64_MAX;
    memcpy(space->captured_fp.fxsave, context->xstate, sizeof(space->captured_fp.fxsave));
    memcpy(&space->fp_mxcsr_mask, context->xstate + 28, sizeof(space->fp_mxcsr_mask));
    space->native_stopped = true;
    space->running = false;
}

int ph_vm_capture_running(struct ph_vm_space *space) {
    struct pacha_thread_context context;
    uint64_t started, now;

    PH_CHECK(space->running && space->user_started && !space->pending);
    PH_OK(ph_task_ops.monotonic_ns(&started));
    for (;;) {
        long status = pacha_syscall2(PACHA_PROCESS_SYSCALL_STOP, space->process_fd, 19);

        if (status) {
            return control_error(space, status);
        }
        status = thread_context(space, PACHA_THREAD_CONTEXT_GET, &context);
        uint64_t event_sequence = atomic_load_explicit(&space->control->event_sequence,
            memory_order_acquire);
        bool event_ready = event_sequence == space->sequence + space->bootstrap_sequence;
        bool user_ip = !status && context.registers.rip >= space->start &&
            context.registers.rip - space->start < space->length;

        if (!event_ready && user_ip) {
            capture_user_context(space, &context);
            return 0;
        }
        /* Never expose a native handler/LPR trampoline as Linux user state.
         * In particular GET refuses an active native wait or fault return.
         * Continue it so its original event can reach the monitor unchanged. */
        long continued = pacha_syscall2(PACHA_PROCESS_SYSCALL_CONTINUE, space->process_fd, 0);

        if (continued) {
            return control_error(space, continued);
        }
        if (event_ready) {
            return -EBUSY;
        }
        if (status && status != PACHA_SYSCALL_ERR_NOT_READY) {
            return control_error(space, status);
        }
        PH_OK(ph_task_ops.monotonic_ns(&now));
        if (now - started >= UINT64_C(5000000000)) {
            return -ETIMEDOUT;
        }
        /* Let a transient bootstrap instruction retire before stopping it
         * again. This is native scheduling, never a hosted Linux task wait. */
        const uint64_t delay[2] = {0, 1000};

        PH_OK(pacha_syscall2(PACHA_RUNTIME_SYSCALL_NANOSLEEP, (uintptr_t)delay, 0));
    }
}

int ph_vm_install_user_context(struct ph_vm_space *space,
    const struct kobox_x86_user_regs *registers, const struct kobox_x86_fp_state *fp) {
    PH_CHECK(space->native_stopped);
    if (!ph_vm_valid_registers(registers) || !ph_vm_valid_fp(space, fp)) {
        return -EINVAL;
    }
    struct pacha_thread_context context = space->initial_context;

    set_user_context(&context, registers, fp);
    long native = thread_context(space, PACHA_THREAD_CONTEXT_SET, &context);
    int result = native ? control_error(space, native) : 0;

    if (!result) {
        space->control->linux_fs_base = registers->fs_base;
        space->control->linux_gs_base = registers->gs_base;
    }
    return result;
}

int ph_vm_start(void *opaque, const struct kobox_x86_user_regs *registers,
    const struct kobox_x86_fp_state *fp) {
    struct ph_vm_space *space = opaque;

    if (!registers || !fp) {
        return -EINVAL;
    }
    uint64_t mask = ph_vm_lock_space(space);
    int result = ph_vm_idle_status(space, space->sequence);

    if (!result && !space->syscalls_enabled) {
        result = -EPERM;
    }
    if (!result && (space->user_started || space->sequence || space->syscall_sequence ||
        space->fault_stopped || space->syscall_stopped || !space->native_stopped)) {
        result = -EBUSY;
    }
    if (!result && (!ph_vm_valid_registers(registers) || !ph_vm_valid_fp(space, fp) ||
        registers->ip < space->start || registers->ip - space->start >= space->length ||
        registers->sp < space->start || registers->sp - space->start > space->length)) {
        result = -EINVAL;
    }
    if (!result) {
        result = ph_vm_install_user_context(space, registers, fp);
        if (!result) {
            space->captured_registers = *registers;
            space->captured_fp = *fp;
            space->control->linux_fs_base = registers->fs_base;
            space->control->linux_gs_base = registers->gs_base;
            space->control->autonomous = 1;
            space->user_started = true;
        }
    }
    ph_vm_unlock_space(space, mask);
    return result;
}
