#include "../userland/personality/linux/runtime/lpr_clock_entry.h"
.text
#define LPR_CLOCK_ENTRY_TEST 1
#include "../userland/personality/linux/runtime/lpr_clock_entry.inc"
.globl lpr_syscall_entry_slow
lpr_syscall_entry_slow:
    incl clock_test_fallbacks(%rip)
    mov $-38,%rax
    jmp *%rcx
.globl clock_test_call
.type clock_test_call,@function
clock_test_call:
    mov %rdi,%rax
    mov %rsi,%rdi
    mov %rdx,%rsi
    movabs $0x123456789abcdef0,%rdx
    movabs $0x23456789abcdef01,%r8
    movabs $0x3456789abcdef012,%r9
    movabs $0x456789abcdef0123,%r10
    fxsave64 clock_test_before(%rip)
    lea .Ltest_return(%rip),%rcx
    jmp lpr_clock_syscall_leaf
.Ltest_return:
    fxsave64 clock_test_after(%rip)
    movabs $0x123456789abcdef0,%r11
    cmp %r11,%rdx
    jne .Ltest_bad
    movabs $0x23456789abcdef01,%r11
    cmp %r11,%r8
    jne .Ltest_bad
    movabs $0x3456789abcdef012,%r11
    cmp %r11,%r9
    jne .Ltest_bad
    movabs $0x456789abcdef0123,%r11
    cmp %r11,%r10
    jne .Ltest_bad
    ret
.Ltest_bad:
    mov $-7777,%rax
    ret
.size clock_test_call,.-clock_test_call
.section .note.GNU-stack,"",@progbits
