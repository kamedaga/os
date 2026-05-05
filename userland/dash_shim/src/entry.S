.section .text
.global _start
_start:
    andq $-16, %rsp
    call dash_shim_main
1:
    pause
    jmp 1b
