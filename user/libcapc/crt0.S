.section .text
.global _start
.type _start, @function
_start:
    xor %rbp, %rbp
    xor %edi, %edi
    xor %esi, %esi
    xor %edx, %edx
    call main

.Lhalt_forever:
    hlt
    jmp .Lhalt_forever
.size _start, . - _start
