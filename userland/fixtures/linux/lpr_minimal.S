.section .text
.globl _start
.type _start, @function
_start:
    mov $39, %rax
    syscall
    test %rax, %rax
    mov $10, %rdi
    js fail_code

    mov $186, %rax
    syscall
    test %rax, %rax
    mov $11, %rdi
    js fail_code

    mov $257, %rax
    mov $-100, %rdi
    lea self_path(%rip), %rsi
    xor %rdx, %rdx
    xor %r10, %r10
    syscall
    test %rax, %rax
    mov $20, %rdi
    js fail_code
    mov %rax, %r12

    mov $5, %rax
    mov %r12, %rdi
    lea statbuf(%rip), %rsi
    syscall
    test %rax, %rax
    mov $21, %rdi
    js fail_code

    mov $0, %rax
    mov %r12, %rdi
    lea readbuf(%rip), %rsi
    mov $32, %rdx
    syscall
    cmp $1, %rax
    mov $22, %rdi
    jl fail_code

    mov $9, %rax
    xor %rdi, %rdi
    mov $4096, %rsi
    mov $1, %rdx
    mov $2, %r10
    mov %r12, %r8
    xor %r9, %r9
    syscall
    test %rax, %rax
    mov $24, %rdi
    js fail_code
    mov %rax, %r13
    cmpb $0x7f, 0(%r13)
    mov $25, %rdi
    jne fail_code
    cmpb $'E', 1(%r13)
    jne fail_code
    cmpb $'L', 2(%r13)
    jne fail_code
    cmpb $'F', 3(%r13)
    jne fail_code

    mov $11, %rax
    mov %r13, %rdi
    mov $4096, %rsi
    syscall
    test %rax, %rax
    mov $26, %rdi
    js fail_code

    mov $3, %rax
    mov %r12, %rdi
    syscall
    test %rax, %rax
    mov $23, %rdi
    js fail_code

    mov $257, %rax
    mov $-100, %rdi
    lea cmd_path(%rip), %rsi
    mov $65536, %rdx
    xor %r10, %r10
    syscall
    test %rax, %rax
    mov $30, %rdi
    js fail_code
    mov %rax, %r12

    mov $217, %rax
    mov %r12, %rdi
    lea dentbuf(%rip), %rsi
    mov $4096, %rdx
    syscall
    cmp $1, %rax
    mov $31, %rdi
    jl fail_code

    mov $3, %rax
    mov %r12, %rdi
    syscall
    test %rax, %rax
    mov $32, %rdi
    js fail_code

    mov $1, %rax
    mov $1, %rdi
    lea ok_msg(%rip), %rsi
    mov $ok_msg_len, %rdx
    syscall
    cmp $ok_msg_len, %rax
    mov $40, %rdi
    jne fail_code

    mov $231, %rax
    xor %rdi, %rdi
    syscall

fail:
    mov $1, %rdi
fail_code:
    mov $60, %rax
    syscall
    hlt

.section .rodata
ok_msg:
    .ascii "[lpr-minimal] OK\n"
.set ok_msg_len, . - ok_msg
self_path:
    .asciz "/cmd/lpr_minimal_linux.elf"
cmd_path:
    .asciz "/cmd"

.section .bss
.balign 16
statbuf:
    .skip 256
readbuf:
    .skip 64
dentbuf:
    .skip 4096
