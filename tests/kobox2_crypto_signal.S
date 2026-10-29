/* SPDX-License-Identifier: MIT */
/* Linux x86-64 ABI, deliberately limited to the FXSAVE512 contract. */
.text
.global crypto_signal_fp_probe
.type crypto_signal_fp_probe,@function
crypto_signal_fp_probe:
    pushq %r12
    subq $1552, %rsp
    fxsave64 (%rsp)
    fninit
    fld1
    fldpi
    ldmxcsr .Lprobe_mxcsr(%rip)
    .irp reg,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
        movdqa .Lfp_seed(%rip), %xmm\reg
        pslld $\reg, %xmm\reg
    .endr
    fxsave64 512(%rsp)
    /* tgkill(pid, tid, signo): signal must be delivered before return. */
    movl $234, %eax
    syscall
    movq %rax, 1536(%rsp)
    fxsave64 1024(%rsp)
    movl $1, %r12d
    cmpq $0, 1536(%rsp)
    jne .Lrestore
    movzwl 512(%rsp), %eax
    cmpw %ax, 1024(%rsp)
    jne .Lrestore
    movb 516(%rsp), %al
    cmpb %al, 1028(%rsp)
    jne .Lrestore
    movl 536(%rsp), %eax
    cmpl %eax, 1048(%rsp)
    jne .Lrestore
    /* Compare both live 80-bit x87 values, excluding padding/FIP fields. */
    leaq 544(%rsp), %rsi
    leaq 1056(%rsp), %rdi
    movl $10, %ecx
    repe cmpsb
    jne .Lrestore
    leaq 560(%rsp), %rsi
    leaq 1072(%rsp), %rdi
    movl $10, %ecx
    repe cmpsb
    jne .Lrestore
    /* All sixteen 128-bit XMM registers must survive the handler. */
    leaq 672(%rsp), %rsi
    leaq 1184(%rsp), %rdi
    movl $256, %ecx
    repe cmpsb
    jne .Lrestore
    xorl %r12d, %r12d
.Lrestore:
    fxrstor64 (%rsp)
    movl %r12d, %eax
    addq $1552, %rsp
    popq %r12
    ret
.size crypto_signal_fp_probe, .-crypto_signal_fp_probe

.global crypto_signal_clobber_fp
.type crypto_signal_clobber_fp,@function
crypto_signal_clobber_fp:
    fninit
    fldcw .Lhandler_fcw(%rip)
    ldmxcsr .Lhandler_mxcsr(%rip)
    .irp reg,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
        pxor %xmm\reg, %xmm\reg
    .endr
    ret
.size crypto_signal_clobber_fp, .-crypto_signal_clobber_fp
.section .rodata
.balign 16
.Lfp_seed:
    .quad 0x123456789abcdef0, 0x0fedcba987654321
.Lprobe_mxcsr:
    .long 0x3f80
.Lhandler_mxcsr:
    .long 0x5f80
.Lhandler_fcw:
    .word 0x0b7f
.section .note.GNU-stack,"",@progbits
