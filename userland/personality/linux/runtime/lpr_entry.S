.text
.globl lpr_syscall_entry
.hidden lpr_syscall_entry
.type lpr_syscall_entry, @function
lpr_syscall_entry:
    sub $296, %rsp
    lea 24(%rsp), %r11
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
    lea 296(%rsp), %r10
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
    call lpr_dispatch_syscall_frame

    lea 24(%rsp), %r11
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
    sub $8, %rsp
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
    sub $8, %rsp
    call lpr_linux_rt_sigreturn_body
    ud2
.size lpr_async_signal_restorer, .-lpr_async_signal_restorer

.globl lpr_runtime_text_end
.hidden lpr_runtime_text_end
lpr_runtime_text_end:

.section .note.GNU-stack,"",@progbits
