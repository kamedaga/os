/* SPDX-License-Identifier: MIT */
.text
.global notification_fp_invoke
.type notification_fp_invoke,@function
/* fn, arg, seed, observed, installed. Seed/capture bracket the ABI call so that
 * the test compiler cannot clobber a caller-saved vector between them. */
notification_fp_invoke:
    push %r12
    push %r13
    sub $520, %rsp
    mov %rdi, %r12
    mov %rcx, %r13
    mov %rsi, %rdi
    fxsaveq (%rsp)
    fxrstorq (%rdx)
    fxsaveq (%r8)
    call *%r12
    fxsaveq (%r13)
    fxrstorq (%rsp)
    add $520, %rsp
    pop %r13
    pop %r12
    ret
.size notification_fp_invoke, .-notification_fp_invoke
.section .note.GNU-stack,"",@progbits
