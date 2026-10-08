/* SPDX-License-Identifier: MIT */
/* uint64_t probe_switch(uint64_t *save_rsp, uint64_t next_rsp, uint64_t *out)
 * Loads known values into every callee-saved GPR, MXCSR and the x87 control
 * word, switches away, and records them after the switch returns here. */
.text
.global probe_switch
probe_switch:
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15
    push %rdx                      /* out */
    sub $8, %rsp
    stmxcsr (%rsp)                 /* caller's MXCSR/CW, restored at the end */
    fnstcw 4(%rsp)
    sub $16, %rsp
    movl $0x7f80, (%rsp)           /* round toward zero, all masked */
    ldmxcsr (%rsp)
    movw $0x0c7f, 4(%rsp)          /* x87 truncate */
    fldcw 4(%rsp)
    movabs $0x1111111111111111, %rbx
    movabs $0x2222222222222222, %rbp
    movabs $0x3333333333333333, %r12
    movabs $0x4444444444444444, %r13
    movabs $0x5555555555555555, %r14
    movabs $0x6666666666666666, %r15
    call ph_fiber_switch@PLT
    stmxcsr (%rsp)
    fnstcw 4(%rsp)
    mov 24(%rsp), %rax             /* out */
    mov %rbx, 0(%rax)
    mov %rbp, 8(%rax)
    mov %r12, 16(%rax)
    mov %r13, 24(%rax)
    mov %r14, 32(%rax)
    mov %r15, 40(%rax)
    movl (%rsp), %ecx
    mov %rcx, 48(%rax)
    movzwl 4(%rsp), %ecx
    mov %rcx, 56(%rax)
    add $16, %rsp
    ldmxcsr (%rsp)
    fldcw 4(%rsp)
    add $16, %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx
    ret

/* void clobber_and_switch(uint64_t *save_rsp, uint64_t next_rsp)
 * The other fiber: switches away with every callee-saved register and both
 * control words destroyed, as arbitrary Linux code would leave them. Its own
 * C caller's registers are kept around that, as the ABI requires. */
.global clobber_and_switch
clobber_and_switch:
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15
    sub $8, %rsp
    stmxcsr (%rsp)
    fnstcw 4(%rsp)
    sub $8, %rsp
    movl $0x1f80 ^ 0x6000, (%rsp)  /* round toward zero via up+down bits */
    ldmxcsr (%rsp)
    movw $0x0b7f, 4(%rsp)
    fldcw 4(%rsp)
    mov $-1, %rbx
    mov $-1, %rbp
    mov $-1, %r12
    mov $-1, %r13
    mov $-1, %r14
    mov $-1, %r15
    call ph_fiber_switch@PLT
    add $8, %rsp
    ldmxcsr (%rsp)
    fldcw 4(%rsp)
    add $8, %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx
    ret
.section .note.GNU-stack,"",@progbits
