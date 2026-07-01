.text
.globl lpr_syscall_entry
.hidden lpr_syscall_entry
.type lpr_syscall_entry, @function
lpr_syscall_entry:
    push %rdi
    push %rsi
    push %rdx
    push %r10
    push %r8
    push %r9
    sub $8, %rsp
    mov 8(%rsp), %r11
    mov %r11, (%rsp)
    mov %rax, %rdi
    mov 48(%rsp), %rsi
    mov 40(%rsp), %rdx
    mov 32(%rsp), %rcx
    mov 24(%rsp), %r8
    mov 16(%rsp), %r9
    call lpr_dispatch_syscall
    add $8, %rsp
    pop %r9
    pop %r8
    pop %r10
    pop %rdx
    pop %rsi
    pop %rdi
    ret
.size lpr_syscall_entry, .-lpr_syscall_entry

.globl lpr_trap_entry
.hidden lpr_trap_entry
.type lpr_trap_entry, @function
lpr_trap_entry:
    jmp lpr_syscall_entry
    ret
.size lpr_trap_entry, .-lpr_trap_entry

.section .note.GNU-stack,"",@progbits
