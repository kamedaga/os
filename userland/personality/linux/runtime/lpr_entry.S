.text
.globl lpr_syscall_entry
.hidden lpr_syscall_entry
.type lpr_syscall_entry, @function
lpr_syscall_entry:
    lea lpr_entry_user_frame(%rip), %r11
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
    mov %rsp, 128(%r11)
    movq $0x202, 136(%r11)

    sub $24, %rsp
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

    add $24, %rsp
    lea lpr_entry_user_frame(%rip), %r11
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

.globl lpr_trap_entry
.hidden lpr_trap_entry
.type lpr_trap_entry, @function
lpr_trap_entry:
    jmp lpr_syscall_entry
    ret
.size lpr_trap_entry, .-lpr_trap_entry

.section .note.GNU-stack,"",@progbits

.bss
.p2align 3
.hidden lpr_entry_user_frame
lpr_entry_user_frame:
    .zero 144
