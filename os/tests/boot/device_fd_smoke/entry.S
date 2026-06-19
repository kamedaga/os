.section .text
.global _start
.type _start,@function
_start:
    andq $-16, %rsp
    call device_fd_boot_smoke_main
1:
    pause
    jmp 1b
