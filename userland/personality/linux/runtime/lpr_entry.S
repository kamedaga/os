#include <personality/lpr_signal_stack.h>
#include <pacha/abi.h>
#include "lpr_entry_stack.h"

.text

.globl lpr_syscall_entry
.hidden lpr_syscall_entry
.type lpr_syscall_entry, @function
lpr_syscall_entry:
    # Only two transit words touch the caller stack. Capture on an LPR stack
    # before any C call: a Go goroutine can have just 2 KiB of stack in total.
    push %rax
    push %rcx
    mov $PACHA_RUNTIME_SYSCALL_GETTID, %eax
    syscall
    mov %rax, %r11
.Lentry_lock:
    mov $1, %eax
    xchg %eax, lpr_entry_context_lock(%rip)
    test %eax, %eax
    jz .Lentry_find
    pause
    jmp .Lentry_lock
.Lentry_find:
    mov lpr_entry_context_head(%rip), %rcx
.Lentry_next:
    test %rcx, %rcx
    jz .Lentry_bootstrap
    cmp %r11, 8(%rcx)
    je .Lentry_found
    mov 0(%rcx), %rcx
    jmp .Lentry_next
.Lentry_found:
    mov 16(%rcx), %rax
    movl $0, lpr_entry_context_lock(%rip)
    jmp .Lentry_switch
.Lentry_bootstrap:
    movl $0, lpr_entry_context_lock(%rip)
.Lbootstrap_lock:
    mov $1, %eax
    xchg %eax, lpr_entry_bootstrap_lock(%rip)
    test %eax, %eax
    jz .Lbootstrap_stack
    pause
    jmp .Lbootstrap_lock
.Lbootstrap_stack:
    lea lpr_entry_bootstrap_stack+LPR_ENTRY_BOOTSTRAP_BYTES(%rip), %rax
.Lentry_switch:
    mov %rsp, %r11
    mov %rax, %rsp
    mov 8(%r11), %rax
    mov 0(%r11), %rcx
    lea 16(%r11), %r11
    jmp lpr_syscall_entry_private
.size lpr_syscall_entry, .-lpr_syscall_entry

#define LPR_SYSCALL_PRESERVE_FP 1
#define LPR_SYSCALL_ENTRY_NAME lpr_syscall_entry_private
#define LPR_SYSCALL_ENTRY_PRIVATE_STACK 1
#include "lpr_syscall_entry.inc"

.globl lpr_fork_child_entry
.hidden lpr_fork_child_entry
.type lpr_fork_child_entry, @function
lpr_fork_child_entry:
    call lpr_fork_child_bootstrap
.Lrestore_clone_child:
    mov %rax, %r11
    mov 152(%r11), %rcx
    test %rcx, %rcx
    jz .Lclone_fp_restored
    fxrstor64 (%rcx)
.Lclone_fp_restored:
    mov 120(%r11), %rcx
    mov 0(%r11), %r15
    mov 8(%r11), %r14
    mov 16(%r11), %r13
    mov 24(%r11), %r12
    mov 32(%r11), %rbp
    mov 40(%r11), %rbx
    mov 56(%r11), %r10
    mov 64(%r11), %r9
    mov 72(%r11), %r8
    mov 80(%r11), %rdi
    mov 88(%r11), %rsi
    mov 96(%r11), %rdx
    mov 128(%r11), %rsp
    xor %eax, %eax
    jmp *%rcx
.size lpr_fork_child_entry, .-lpr_fork_child_entry

.globl lpr_clone_thread_entry
.hidden lpr_clone_thread_entry
.type lpr_clone_thread_entry, @function
lpr_clone_thread_entry:
    mov (%rsp), %rdi
    call lpr_clone_thread_bootstrap
    # No further bootstrap-stack access occurs after this publication.
    movl $0, lpr_entry_bootstrap_lock(%rip)
    jmp .Lrestore_clone_child
.size lpr_clone_thread_entry, .-lpr_clone_thread_entry

.globl lpr_trap_entry
.hidden lpr_trap_entry
.type lpr_trap_entry, @function
lpr_trap_entry:
    jmp lpr_syscall_entry
    ret
.size lpr_trap_entry, .-lpr_trap_entry

.globl lpr_async_signal_entry
.hidden lpr_async_signal_entry
.type lpr_async_signal_entry, @function
lpr_async_signal_entry:
    # Interrupted Linux RBP is arbitrary, not an owned LPR frame chain.
    # Its original value is already retained in the native signal frame.
    xor %ebp, %ebp
    and $-16, %rsp
    call lpr_linux_async_signal_prepare
.globl lpr_signal_enter_handler
.hidden lpr_signal_enter_handler
lpr_signal_enter_handler:
    mov 16(%rax), %r11
    mov 8(%rax), %rdi
    lea 1056(%rax), %rsi
    lea 1184(%rax), %rdx
    lea -8(%rax), %rsp
    jmp *%r11
.size lpr_async_signal_entry, .-lpr_async_signal_entry

.globl lpr_fault_entry
.hidden lpr_fault_entry
.type lpr_fault_entry, @function
lpr_fault_entry:
    # Fault handlers run on a dedicated stack. Never follow interrupted
    # Linux frame pointers into the unavailable page that caused the fault.
    xor %ebp, %ebp
    and $-16, %rsp
    call lpr_linux_fault_prepare
    ud2
.size lpr_fault_entry, .-lpr_fault_entry

.globl lpr_async_signal_restorer
.hidden lpr_async_signal_restorer
.type lpr_async_signal_restorer, @function
lpr_async_signal_restorer:
    xor %ebp, %ebp
    mov %rsp, %rdi
    and $-16, %rsp
    call lpr_linux_rt_sigreturn_body
    ud2
.size lpr_async_signal_restorer, .-lpr_async_signal_restorer

.globl lpr_deliver_pending_frame
.hidden lpr_deliver_pending_frame
.type lpr_deliver_pending_frame, @function
lpr_deliver_pending_frame:
    push %rbx
    push %r12
    mov %rdi, %rbx
    mov %rsi, %r12
.Lpending_retry:
    test %r12, %r12
    jz .Lpending_fp_ready
    fxrstor64 (%r12)
.Lpending_fp_ready:
    mov $PACHA_PROCESS_SIGNAL_CTL_DELIVER_PENDING_FRAME, %edi
    mov %rbx, %rsi
    xor %edx, %edx
    xor %r10d, %r10d
    xor %r8d, %r8d
    xor %r9d, %r9d
    mov $PACHA_PROCESS_SYSCALL_SIGNAL_CTL, %eax
    syscall
    cmp $-4096, %rax
    jne .Lpending_done
    # Match the native pager retry contract. A user read faults in the page;
    # retry restores FP again before the kernel can snapshot the user state.
    movb (%rdx), %al
    jmp .Lpending_retry
.Lpending_done:
    pop %r12
    pop %rbx
    ret
.size lpr_deliver_pending_frame, .-lpr_deliver_pending_frame

.globl lpr_runtime_text_end
.hidden lpr_runtime_text_end
lpr_runtime_text_end:

.section .note.GNU-stack,"",@progbits
