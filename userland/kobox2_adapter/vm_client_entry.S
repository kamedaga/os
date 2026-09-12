/* SPDX-License-Identifier: MIT */
#include <pacha/abi.h>
#include <personality/lpr_signal_stack.h>
#include "vm_layout.h"
.text
.global _start
.type _start,@function
_start:
    and $-16, %rsp
    call ph_vm_client_main
    ud2
.size _start, .-_start

.global ph_vm_client_fault_entry
.type ph_vm_client_fault_entry,@function
ph_vm_client_fault_entry:
    mov %rdi, %r12
    call ph_vm_client_fault
    mov %r12, %rsi
    mov $PACHA_PROCESS_SIGNAL_CTL_RETURN, %edi
    mov $PACHA_PROCESS_SYSCALL_SIGNAL_CTL, %eax
    syscall
    ud2
.size ph_vm_client_fault_entry, .-ph_vm_client_fault_entry

/* Fixture frontend for the same capture entry used by the LPR zpoline shim.
 * This tests LPR dispatch transport, not executable rewriting. In particular,
 * out-of-range syscall numbers still enter LPR instead of call *rax faulting.
 * The Linux ABI clobbers RCX/R11; every other register is captured by LPR. */
.global ph_vm_client_linux_call
.type ph_vm_client_linux_call,@function
ph_vm_client_linux_call:
    mov %rdi, %rax
    mov %rsi, %r11
    mov 0(%r11), %rdi
    mov 8(%r11), %rsi
    mov 16(%r11), %rdx
    mov 24(%r11), %r10
    mov 32(%r11), %r8
    mov 40(%r11), %r9
    lea .Llinux_return(%rip), %rcx
    jmp ph_vm_client_syscall_entry
.Llinux_return:
    ret
.size ph_vm_client_linux_call, .-ph_vm_client_linux_call

/* zpoline has already put the post-SYSCALL IP in RCX and restored guest RSP.
 * Save FP before entering the shared capture code or any native helper. */
.global ph_vm_client_syscall_entry
.type ph_vm_client_syscall_entry,@function
ph_vm_client_syscall_entry:
    /* Each native process owns one Linux execution context and its private
     * bootstrap stack. Dispatch must not use or fault on the Linux stack. */
    mov %rsp, ph_vm_client_user_sp(%rip)
    lea ph_vm_client_dispatch_stack_end(%rip), %rsp
    pushfq
    popq ph_vm_client_user_flags(%rip)
    fxsave64 ph_vm_client_saved_fp(%rip)
    jmp lpr_syscall_entry
.size ph_vm_client_syscall_entry, .-ph_vm_client_syscall_entry

#define LPR_SYSCALL_RETURN_ENTRY ph_vm_client_return
#include "lpr_syscall_entry.inc"

.type ph_vm_client_return,@function
ph_vm_client_return:
    /* Common capture has restored the other GPRs. A native private slot
     * provides the final IP without sacrificing Linux's RCX or R11. */
    pushq 136(%r11)
    popfq
    mov 112(%r11), %rax
    mov 104(%r11), %rcx
    mov 128(%r11), %rsp
    mov 48(%r11), %r11
    jmp *ph_vm_client_return_ip(%rip)
.size ph_vm_client_return, .-ph_vm_client_return

.bss
.balign 4096
ph_vm_client_dispatch_stack:
    .skip PH_VM_CLIENT_STACK_SIZE
ph_vm_client_dispatch_stack_end:
.section .note.GNU-stack,"",@progbits
