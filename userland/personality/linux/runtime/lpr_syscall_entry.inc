/* SPDX-License-Identifier: MIT */
/* Shared syscall capture/return entry. The embedding runtime owns dispatch. */
#ifndef LPR_SYSCALL_ENTRY_NAME
#define LPR_SYSCALL_ENTRY_NAME lpr_syscall_entry
#endif
.globl LPR_SYSCALL_ENTRY_NAME
.hidden LPR_SYSCALL_ENTRY_NAME
.type LPR_SYSCALL_ENTRY_NAME, @function
LPR_SYSCALL_ENTRY_NAME:
    # Native signal delivery addresses its frame and runtime stack relative to
    # the interrupted Linux RSP, then its preparation code grows downward from
    # that runtime RSP. Keep the LPR C stack below both regions; otherwise
    # signal preparation can overwrite a live dispatch stack before returning.
    # A Linux syscall site does not promise SysV call-stack alignment. Preserve
    # its exact RSP for return, but align the private LPR stack before entering C.
#ifndef LPR_SYSCALL_ENTRY_PRIVATE_STACK
    mov %rsp, %r11
#endif
    sub $LPR_SYSCALL_ENTRY_RESERVE_BYTES, %rsp
    and $-16, %rsp
    mov %r11, 40(%rsp)
    lea 56(%rsp), %r11
    mov %r15, 0(%r11)
    mov %r14, 8(%r11)
    mov %r13, 16(%r11)
    mov %r12, 24(%r11)
    mov %rbp, 32(%r11)
    mov %rbx, 40(%r11)
    movq $0x202, 48(%r11)
    mov %r10, 56(%r11)
    mov %r9, 64(%r11)
    mov %r8, 72(%r11)
    mov %rdi, 80(%r11)
    mov %rsi, 88(%r11)
    mov %rdx, 96(%r11)
    mov %rax, 112(%r11)
    mov %rcx, 104(%r11)
    mov %rcx, 120(%r11)
    mov 40(%rsp), %r10
    mov %r10, 128(%r11)
    movq $0x202, 136(%r11)
    movq $0, 144(%r11)
    movq $0, 152(%r11)
#ifdef LPR_SYSCALL_PRESERVE_FP
    # Linux syscalls preserve FP/SIMD state, unlike a C function call. The
    # LPR C runtime uses baseline SSE2; legacy SSE leaves YMM upper halves
    # untouched. Keep this aligned per-call image outside the GPR frame.
    lea 224(%rsp), %r10
    mov %r10, 152(%r11)
    fxsave64 (%r10)
#endif

#ifdef LPR_SYSCALL_ENTRY_PRIVATE_STACK
    mov %rsp, %rdi
    call lpr_entry_stack_prepare
    mov %rax, %rsp
    test %rdx, %rdx
    jz .Lentry_stack_ready
    # The helper has returned and RSP is private before another initializer
    # can acquire and reuse the shared bootstrap stack.
    movl $0, lpr_entry_bootstrap_lock(%rip)
.Lentry_stack_ready:
    lea 56(%rsp), %r11
#endif
    mov 72(%r11), %r10
    mov %r10, 0(%rsp)
    mov 64(%r11), %r10
    mov %r10, 8(%rsp)
    mov %r11, %rdi
    mov 112(%r11), %rsi
    mov 80(%r11), %rdx
    mov 88(%r11), %rcx
    mov 96(%r11), %r8
    mov 56(%r11), %r9
    movabs $0x4c50524652414d45, %r10
    mov %r10, 16(%rsp)
    mov %r11, 24(%rsp)
    movabs $0x454d41524652504c, %r10
    mov %r10, 32(%rsp)
    lea 16(%rsp), %rbp
    call lpr_dispatch_syscall_frame

    lea 56(%rsp), %r11
#ifdef LPR_SYSCALL_PRESERVE_FP
    mov 152(%r11), %r10
    fxrstor64 (%r10)
#endif
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
#ifdef LPR_SYSCALL_RETURN_ENTRY
    jmp LPR_SYSCALL_RETURN_ENTRY
#else
    mov 128(%r11), %rsp
    jmp *%rcx
#endif
.size LPR_SYSCALL_ENTRY_NAME, .-LPR_SYSCALL_ENTRY_NAME
