/* SPDX-License-Identifier: MIT */
#include <pacha/abi.h>
#include "stack_layout.h"
.text
.global _start
_start:
    /* Relocate the freestanding launcher before any ordinary C entry. */
    and $-16, %rsp
    lea __ehdr_start(%rip), %rdi
    call ph_relocate_self
    /* Known native bounds let Linux reject direct DMA of stack objects. */
    lea ph_boot_stack_end(%rip), %rsp
    call ph_main
    ud2
.global ph_thread_entry
ph_thread_entry:
    call ph_thread_body
    ud2
.pushsection ph_trampoline,"ax",@progbits
.global ph_notification_entry
ph_notification_entry:
    /* r12 is callee-saved across C; the native frame restores its old value. */
    mov %rdi, %r12
    call ph_notification_body
    mov %r12, %rsi
    mov $PACHA_PROCESS_SIGNAL_CTL_RETURN, %edi
    mov $PACHA_PROCESS_SYSCALL_SIGNAL_CTL, %eax
    syscall
    ud2
.popsection
.bss
.balign 4096
.global ph_boot_stack
ph_boot_stack:
    .skip PH_THREAD_STACK_SIZE
ph_boot_stack_end:
.section .note.GNU-stack,"",@progbits
