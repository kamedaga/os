/* SPDX-License-Identifier: MIT */
/* Clone native execution contexts; Linux alone creates tasks, files and mm. */
#include "vm_internal.h"
#include <errno.h>

int ph_vm_clone(void *opaque, uint64_t sequence, uint64_t syscall_sequence,
    bool share_mm, void **output, struct kobox_x86_fp_state *fp) {
    struct ph_vm_space *parent = opaque;

    if (!output || !fp) {
        return -EINVAL;
    }
    uint64_t mask = ph_vm_lock_space(parent);
    int result = ph_vm_captured_status(parent, sequence);
    struct kobox_x86_user_regs registers;
    struct kobox_x86_fp_state captured_fp;

    if (!result && (!parent->syscall_stopped || parent->syscall_returned)) {
        result = -EBUSY;
    }
    if (!result && syscall_sequence != parent->syscall_sequence) {
        result = -ESTALE;
    }
    if (!result) {
        registers = parent->captured_registers;
        captured_fp = parent->captured_fp;
    }
    ph_vm_unlock_space(parent, mask);
    if (result) {
        return result;
    }
    struct ph_vm_space *child = ph_vm_create(parent->bootstrap_file, parent->bootstrap_size,
        parent->start, parent->length);
    child->exec_image = parent->exec_image;

    if (share_mm) {
        result = ph_vm_memory_share(child, parent);
        if (result) {
            ph_vm_destroy(child);
            return result;
        }
    }
    child->captured_registers = registers;
    child->captured_fp = captured_fp;
    child->syscalls_enabled = true;
    child->syscall_stopped = true;
    child->syscall_sequence = syscall_sequence;
    child->user_started = true;
    child->control->autonomous = 1;
    child->control->syscall_sequence = syscall_sequence;
    memcpy(child->control->fp, captured_fp.fxsave, sizeof(captured_fp.fxsave));
    child->control->registers = ph_vm_export_registers(&registers);

    /* The caller owns the still-parked child only on success. Common Linux
     * may close it during clone/exec rollback; keep its tombstone until the
     * root launcher knows all Linux bindings and event consumers have left. */
    struct ph_vm_space *owner = parent->owner;

    mask = ph_vm_lock_space(owner);
    child->owner = owner;
    child->owned_next = owner->owned_children;
    owner->owned_children = child;
    ph_vm_unlock_space(owner, mask);
    *fp = captured_fp;
    *output = child;
    return 0;
}
