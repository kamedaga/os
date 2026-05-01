.section .text
.global _start
.type _start,@function
_start:
    andq $-16, %rsp
    call esp_server_main
1:
    pause
    jmp 1b
