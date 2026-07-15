#include <personality/lpr_signal_stack.h>

.text

.globl lpr_syscall_entry
.hidden lpr_syscall_entry
.type lpr_syscall_entry, @function
lpr_syscall_entry:
    # Native signal delivery addresses its frame and runtime stack relative to
    # the interrupted Linux RSP, then its preparation code grows downward from
    # that runtime RSP. Keep the LPR C stack below both regions; otherwise
    # signal preparation can overwrite a live dispatch stack before returning.
    sub $LPR_SYSCALL_ENTRY_RESERVE_BYTES, %rsp
    lea 56(%rsp), %r11
    mov %r15, 0(%r11)
    mov %r14, 8(%r11)
    mov %r13, 16(%r11)
    mov %r12, 24(%r11)
    mov %rbp, 32(%r11)
    mov %rbx, 40(%r11)
    movq $0x202, 48(%r11)
    mov %r10, 56(%r11)
    mov %r9, 64(%r11)
    mov %r8, 72(%r11)
    mov %rdi, 80(%r11)
    mov %rsi, 88(%r11)
    mov %rdx, 96(%r11)
    mov %rax, 112(%r11)
    mov %rcx, 104(%r11)
    mov %rcx, 120(%r11)
    lea LPR_SYSCALL_ENTRY_RESERVE_BYTES(%rsp), %r10
    mov %r10, 128(%r11)
    movq $0x202, 136(%r11)

    mov 72(%r11), %r10
    mov %r10, 0(%rsp)
    mov 64(%r11), %r10
    mov %r10, 8(%rsp)
    mov %r11, %rdi
    mov 112(%r11), %rsi
    mov 80(%r11), %rdx
    mov 88(%r11), %rcx
    mov 96(%r11), %r8
    mov 56(%r11), %r9
    movabs $0x4c50524652414d45, %r10
    mov %r10, 16(%rsp)
    mov %r11, 24(%rsp)
    movabs $0x454d41524652504c, %r10
    mov %r10, 32(%rsp)
    lea 16(%rsp), %rbp
    call lpr_dispatch_syscall_frame

    lea 56(%rsp), %r11
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
    jmp *%rcx
.size lpr_syscall_entry, .-lpr_syscall_entry

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
    lea 736(%rax), %rsi
    lea 864(%rax), %rdx
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
