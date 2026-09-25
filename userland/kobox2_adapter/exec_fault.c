/* SPDX-License-Identifier: MIT */
/* Recover a fault in an authenticated rewritten instruction, not arbitrary
 * CALLs. Linux still decides every syscall result and invalid-frame signal. */
#include "vm_internal.h"
#include "exec_image.h"
#include <elf.h>

bool ph_exec_capture_syscall_fault(struct ph_vm_space *space) {
    struct ph_vm_client_control *control = space->control;
    const struct ph_exec_image *image = space->exec_image;
    unsigned char instruction[2];
    uint64_t ip = control->fault_ip;

    /* A CALL can fault before pushing its return IP, even though the
     * original SYSCALL needs no valid stack (notably rt_sigreturn).
     * ET_DYN needs per-exec load-bias provenance before using this path. */
    if (!image || image->elf_type != ET_EXEC || control->fault_vector != 14 ||
        (control->fault_error & ~UINT64_C(0x17)) ||
        (control->fault_error & 0x16) != 6 ||
        control->fault_address - (control->fault_sp - 8) >= 8 ||
        !ph_vm_read_executable(space, ip, instruction, sizeof(instruction)) ||
        instruction[0] != 0xff || instruction[1] != 0xd0) {
        return false;
    }
    for (size_t index = 0; index < image->site_count; ++index) {
        if (image->syscall_sites[index] != ip) {
            continue;
        }
        PH_CHECK(control->syscall_sequence != UINT64_MAX && ip <= UINT64_MAX - 2);
        control->registers.rip = ip + 2;
        control->registers.rcx = ip + 2;
        control->registers.r11 = control->registers.rflags;
        ++control->syscall_sequence;
        space->syscall_in_fault = true;
        ph_vm_capture_syscall(space);
        return true;
    }
    return false;
}
