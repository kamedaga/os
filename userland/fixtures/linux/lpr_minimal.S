.section .text
.globl _start
.type _start, @function
_start:
    mov $39, %rax
    syscall
    test %rax, %rax
    js fail

    mov $186, %rax
    syscall
    test %rax, %rax
    js fail

    mov $1, %rax
    mov $1, %rdi
    lea ok_msg(%rip), %rsi
    mov $ok_msg_len, %rdx
    syscall
    cmp $ok_msg_len, %rax
    jne fail

    mov $231, %rax
    xor %rdi, %rdi
    syscall

fail:
    mov $60, %rax
    mov $1, %rdi
    syscall
    hlt

.section .rodata
ok_msg:
    .ascii "[lpr-minimal] OK\n"
.set ok_msg_len, . - ok_msg
