/* SPDX-License-Identifier: MIT */
/* Captured LPR calls. Linux dispatch and FD ownership stay in the core. */
#include "vm_internal.h"
#include <errno.h>

void ph_vm_capture_syscall(struct ph_vm_space *space) {
    const struct ph_vm_client_control *control = space->control;

    PH_CHECK(space->syscalls_enabled && !space->syscall_stopped);
    PH_CHECK(space->syscall_sequence != UINT64_MAX);
    PH_CHECK(control->syscall_sequence == space->syscall_sequence + 1);
    space->syscall_sequence = control->syscall_sequence;
    space->captured_registers = ph_vm_import_registers(&control->registers);
    memcpy(space->captured_fp.fxsave, control->fp, sizeof(control->fp));
    memcpy(&space->fp_mxcsr_mask, control->fp + 28, sizeof(space->fp_mxcsr_mask));
    space->syscall_stopped = true;
    space->syscall_returned = false;
    space->event.kind = KOBOX_VM_EVENT_SYSCALL;
    space->event.user = space->captured_registers;
    space->event.syscall = (struct kobox_linux_vm_syscall) {
        .sequence = space->syscall_sequence,
        .number = control->registers.rax,
        .arguments = {
            control->registers.rdi, control->registers.rsi, control->registers.rdx,
            control->registers.r10, control->registers.r8, control->registers.r9,
        },
    };
}

int ph_vm_enable_syscalls(void *opaque) {
    struct ph_vm_space *space = opaque;
    uint64_t mask = ph_vm_lock_space(space);
    int result = ph_vm_idle_status(space, space->sequence);

    if (!result) {
        space->syscalls_enabled = true;
    }
    ph_vm_unlock_space(space, mask);
    return result;
}

int ph_vm_issue_syscall(void *opaque, uint64_t number,
    const uint64_t arguments[6], uint64_t sequence) {
    struct ph_vm_space *space = opaque;

    if (!arguments) {
        return -EINVAL;
    }
    uint64_t mask = ph_vm_lock_space(space);
    int result = ph_vm_idle_status(space, sequence);

    if (!result && !space->syscalls_enabled) {
        result = -EPERM;
    }
    if (!result && (space->syscall_stopped || space->fault_stopped)) {
        result = -EBUSY;
    }
    if (!result) {
        space->control->registers = (struct ph_vm_client_registers) {
            .rax = number,
            .rdi = arguments[0], .rsi = arguments[1], .rdx = arguments[2],
            .r10 = arguments[3], .r8 = arguments[4], .r9 = arguments[5],
        };
        ph_vm_publish_command(space, PH_VM_CLIENT_SYSCALL);
    }
    ph_vm_unlock_space(space, mask);
    return result;
}

int ph_vm_syscall_return(void *opaque, uint64_t sequence, uint64_t syscall_sequence,
    const struct kobox_x86_user_regs *registers) {
    struct ph_vm_space *space = opaque;

    if (!registers) {
        return -EINVAL;
    }
    uint64_t mask = ph_vm_lock_space(space);
    int result = ph_vm_captured_status(space, sequence);

    if (!result && !space->syscall_stopped) {
        result = -EBUSY;
    }
    if (!result && syscall_sequence != space->syscall_sequence) {
        result = -ESTALE;
    }
    if (!result && space->syscall_returned) {
        result = -ESTALE;
    }
    if (!result && space->native_stopped && space->user_started) {
        /* A new clone/exec host starts directly at Linux's return image;
         * there is no parent's suspended native dispatch stack to resume. */
        result = ph_vm_install_user_context(space, registers, &space->captured_fp);
    }
    if (!result) {
        space->captured_registers = *registers;
        space->control->registers = ph_vm_export_registers(registers);
        space->syscall_returned = true;
        if (space->syscall_in_fault) {
            space->control->restore_fault = 1;
        }
    }
    ph_vm_unlock_space(space, mask);
    return result;
}
