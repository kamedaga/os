#include "lpr_thread_page.inc"
.text
.globl lpr_thread_page_read
.hidden lpr_thread_page_read
.type lpr_thread_page_read, @function
lpr_thread_page_read:
    LPR_THREAD_READ .Lthread_ok, .Lthread_no
.Lthread_ok:
    mov %r11, %rax
    ret
.Lthread_no:
    xor %eax, %eax
    ret
.size lpr_thread_page_read, .-lpr_thread_page_read
.section .note.GNU-stack,"",@progbits
