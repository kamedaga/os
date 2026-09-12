/* SPDX-License-Identifier: MIT */
/* Captured machine frames; native fault return retains the full XSAVE bank. */
#include "vm_internal.h"
#include <errno.h>

int ph_vm_idle_status(const struct ph_vm_space *space, uint64_t sequence) {
    if (space->reaped) {
        return -ESRCH;
    }
    if (sequence != space->sequence || sequence >= UINT64_MAX - space->bootstrap_sequence) {
        return -ESTALE;
    }
    if (space->running || space->pending) {
        return -EBUSY;
    }
    return 0;
}

struct kobox_x86_user_regs ph_vm_import_registers(const struct ph_vm_client_registers *in) {
    return (struct kobox_x86_user_regs) {
        .ax = in->rax, .bx = in->rbx, .cx = in->rcx, .dx = in->rdx,
        .si = in->rsi, .di = in->rdi, .bp = in->rbp,
        .r8 = in->r8, .r9 = in->r9, .r10 = in->r10, .r11 = in->r11,
        .r12 = in->r12, .r13 = in->r13, .r14 = in->r14, .r15 = in->r15,
        .ip = in->rip, .sp = in->rsp, .flags = in->rflags, .orig_ax = in->rax,
        .fs_base = in->fs_base, .gs_base = in->gs_base,
        /* The fixed core's long-mode user selectors, not native selectors. */
        .cs = 0x33, .ss = 0x2b,
    };
}

struct ph_vm_client_registers ph_vm_export_registers(const struct kobox_x86_user_regs *in) {
    return (struct ph_vm_client_registers) {
        .rax = in->ax, .rbx = in->bx, .rcx = in->cx, .rdx = in->dx,
        .rsi = in->si, .rdi = in->di, .rbp = in->bp,
        .r8 = in->r8, .r9 = in->r9, .r10 = in->r10, .r11 = in->r11,
        .r12 = in->r12, .r13 = in->r13, .r14 = in->r14, .r15 = in->r15,
        .rip = in->ip, .rsp = in->sp, .rflags = in->flags,
        .fs_base = in->fs_base, .gs_base = in->gs_base,
    };
}

void ph_vm_capture_fault(struct ph_vm_space *space) {
    PH_CHECK(!space->fault_stopped && !space->syscall_stopped);
    space->captured_registers = ph_vm_import_registers(&space->control->registers);
    space->captured_registers.orig_ax = UINT64_MAX;
    memcpy(space->captured_fp.fxsave, space->control->fp, sizeof(space->control->fp));
    memcpy(&space->fp_mxcsr_mask, space->control->fp + 28, sizeof(space->fp_mxcsr_mask));
    space->fault_stopped = true;
    space->event.user = space->captured_registers;
}

int ph_vm_captured_status(const struct ph_vm_space *space, uint64_t sequence) {
    int result = ph_vm_idle_status(space, sequence);

    if (!result && !space->syscall_stopped && !space->fault_stopped &&
        !(space->native_stopped && space->user_started)) {
        result = -EOPNOTSUPP;
    }
    return result;
}

bool ph_vm_valid_fp(const struct ph_vm_space *space, const struct kobox_x86_fp_state *fp) {
    uint32_t mxcsr, mask;

    /* Use the captured CPU's capability mask, never the submitted image's
     * mask. FXSAVE defines these architectural fields at bytes 24 and 28. */
    memcpy(&mxcsr, fp->fxsave + 24, sizeof(mxcsr));
    mask = space->fp_mxcsr_mask;
    if (!mask) {
        mask = 0xffbf;
    }
    return !(mxcsr & ~mask);
}

bool ph_vm_valid_registers(const struct kobox_x86_user_regs *registers) {
    return registers->cs == 0x33 && registers->ss == 0x2b &&
        !registers->ds && !registers->es && !registers->fs && !registers->gs &&
        registers->ip < (UINT64_C(1) << 47) && registers->sp < (UINT64_C(1) << 47) &&
        registers->fs_base < (UINT64_C(1) << 47) &&
        registers->gs_base < (UINT64_C(1) << 47);
}

int ph_vm_restore(void *opaque, uint64_t sequence,
    const struct kobox_x86_user_regs *registers, const struct kobox_x86_fp_state *fp) {
    struct ph_vm_space *space = opaque;

    if (!registers || !fp) {
        return -EINVAL;
    }
    uint64_t mask = ph_vm_lock_space(space);
    int result = ph_vm_captured_status(space, sequence);

    if (!result && !space->fault_stopped &&
        !(space->native_stopped && !space->syscall_stopped) &&
        !(space->syscall_stopped && space->syscall_returned)) {
        result = -EBUSY;
    }
    if (!result && (!ph_vm_valid_registers(registers) || !ph_vm_valid_fp(space, fp))) {
        result = -EINVAL;
    }
    if (!result && space->native_stopped && space->user_started) {
        /* clone/exec return has no suspended native handler to read control.
         * A replacement frame must reach the stopped hardware context too. */
        result = ph_vm_install_user_context(space, registers, fp);
    }
    if (!result) {
        space->captured_registers = *registers;
        space->captured_fp = *fp;
        space->control->registers = ph_vm_export_registers(registers);
        memcpy(space->control->fp, fp->fxsave, sizeof(fp->fxsave));
        if (space->fault_stopped) {
            space->control->restore_fault = 1;
        }
    }
    ph_vm_unlock_space(space, mask);
    return result;
}

int ph_vm_snapshot(void *opaque, uint64_t sequence,
    struct kobox_x86_user_regs *registers, struct kobox_x86_fp_state *fp) {
    struct ph_vm_space *space = opaque;

    if (!registers || !fp) {
        return -EINVAL;
    }
    uint64_t mask = ph_vm_lock_space(space);
    int result = ph_vm_captured_status(space, sequence);

    if (result == -EBUSY && space->running && !space->pending && space->user_started) {
        result = ph_vm_capture_running(space);
    }
    if (!result) {
        *registers = space->captured_registers;
        *fp = space->captured_fp;
    }
    ph_vm_unlock_space(space, mask);
    return result;
}

int ph_vm_write_fpregs(void *opaque, uint64_t sequence, const struct kobox_x86_fp_state *fp) {
    struct ph_vm_space *space = opaque;

    if (!fp) {
        return -EINVAL;
    }
    uint64_t mask = ph_vm_lock_space(space);
    int result = ph_vm_captured_status(space, sequence);

    if (!result && !ph_vm_valid_fp(space, fp)) {
        result = -EINVAL;
    }
    if (!result && space->native_stopped && space->user_started) {
        result = ph_vm_install_user_context(space, &space->captured_registers, fp);
    }
    if (!result) {
        space->captured_fp = *fp;
        memcpy(space->control->fp, fp->fxsave, sizeof(fp->fxsave));
        if (space->fault_stopped) {
            space->control->restore_fault = 1;
        }
    }
    ph_vm_unlock_space(space, mask);
    return result;
}
