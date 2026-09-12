#include <personality/lpr_signal_stack.h>

.text

#include "lpr_syscall_entry.inc"

.globl lpr_fork_child_entry
.hidden lpr_fork_child_entry
.type lpr_fork_child_entry, @function
lpr_fork_child_entry:
    call lpr_fork_child_bootstrap
    mov %rax, %r11
    mov 120(%r11), %rcx
    mov 0(%r11), %r15
    mov 8(%r11), %r14
    mov 16(%r11), %r13
    mov 24(%r11), %r12
    mov 32(%r11), %rbp
    mov 40(%r11), %rbx
    mov 56(%r11), %r10
    mov 64(%r11), %r9
    mov 72(%r11), %r8
    mov 80(%r11), %rdi
    mov 88(%r11), %rsi
    mov 96(%r11), %rdx
    mov 128(%r11), %rsp
    xor %eax, %eax
    jmp *%rcx
.size lpr_fork_child_entry, .-lpr_fork_child_entry

.globl lpr_clone_thread_entry
.hidden lpr_clone_thread_entry
.type lpr_clone_thread_entry, @function
lpr_clone_thread_entry:
    mov %rsp, %rdi
    call lpr_clone_thread_bootstrap
    ud2
.size lpr_clone_thread_entry, .-lpr_clone_thread_entry

.globl lpr_trap_entry
.hidden lpr_trap_entry
.type lpr_trap_entry, @function
lpr_trap_entry:
    jmp lpr_syscall_entry
    ret
.size lpr_trap_entry, .-lpr_trap_entry

.globl lpr_async_signal_entry
.hidden lpr_async_signal_entry
.type lpr_async_signal_entry, @function
lpr_async_signal_entry:
    and $-16, %rsp
    call lpr_linux_async_signal_prepare
    mov 16(%rax), %r11
    mov 8(%rax), %rdi
    lea 1056(%rax), %rsi
    lea 1184(%rax), %rdx
    lea -8(%rax), %rsp
    jmp *%r11
.size lpr_async_signal_entry, .-lpr_async_signal_entry

.globl lpr_async_signal_restorer
.hidden lpr_async_signal_restorer
.type lpr_async_signal_restorer, @function
lpr_async_signal_restorer:
    mov %rsp, %rdi
    and $-16, %rsp
    call lpr_linux_rt_sigreturn_body
    ud2
.size lpr_async_signal_restorer, .-lpr_async_signal_restorer

.globl lpr_runtime_text_end
.hidden lpr_runtime_text_end
lpr_runtime_text_end:

.section .note.GNU-stack,"",@progbits
