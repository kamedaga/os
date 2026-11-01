.text
.globl lpr_clock_copy_timespec
.hidden lpr_clock_copy_timespec
.type lpr_clock_copy_timespec,@function
lpr_clock_copy_timespec:
.globl lpr_clock_copy_seconds
.hidden lpr_clock_copy_seconds
lpr_clock_copy_seconds:
    movq %rsi, (%rdi)
.globl lpr_clock_copy_nanos
.hidden lpr_clock_copy_nanos
lpr_clock_copy_nanos:
    movq %rdx, 8(%rdi)
    xorl %eax, %eax
    ret
.globl lpr_clock_copy_bad
.hidden lpr_clock_copy_bad
lpr_clock_copy_bad:
    movq $-14, %rax
    ret
.size lpr_clock_copy_timespec,.-lpr_clock_copy_timespec
.section .note.GNU-stack,"",@progbits
