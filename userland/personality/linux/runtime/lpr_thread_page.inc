/* SPDX-License-Identifier: MIT */
/* Only RAX/RCX/R11 may be destroyed: Linux RDX is an argument, FS/GS are
 * application state, and the caller stack has just the existing two words.
 * CPU2 precedes current deliberately. Equal even sequence brackets current;
 * returning to either CPU increments it on EVERY user return. LFENCE orders
 * RDPID against loads and keeps the second sequence after current. Thus ABA
 * migration and a remote writer cannot combine an old sequence with new TID.
 * Bounded retries keep preemption/livelock a GETTID fallback, not a spin. */
.macro LPR_THREAD_ATTEMPT success, failure
    mov lpr_thread_fast_page(%rip), %rax
    test %rax, %rax
    jz \failure
    cmpq $1, 32(%rax)
    jne \failure
    lfence
#ifdef LPR_THREAD_PAGE_CPU_TEST
    mov lpr_thread_test_cpu(%rip), %r11d
#else
    rdpid %r11
#endif
    lfence
    cmp 16(%rax), %r11
    jae \failure
    shl $6, %r11
    lea 64(%rax,%r11), %rax
    mov (%rax), %rcx
    test %rcx, %rcx
    jz \failure
    test $1, %cl
    jnz \failure
    lfence
#ifdef LPR_THREAD_PAGE_CPU_TEST
    mov lpr_thread_test_cpu(%rip), %r11d
#else
    rdpid %r11
#endif
    lfence
    shl $6, %r11
    add lpr_thread_fast_page(%rip), %r11
    add $64, %r11
    cmp %rax, %r11
    jne \failure
    mov 8(%rax), %r11
    lfence
    cmp (%rax), %rcx
    jne \failure
    mov lpr_thread_fast_page(%rip), %rax
    cmpq $1, 32(%rax)
    jne \failure
    test %r11, %r11
    jz \failure
    jmp \success
.endm
.macro LPR_THREAD_READ success, failure
    LPR_THREAD_ATTEMPT \success, .Lthread_retry1\@
.Lthread_retry1\@:
    LPR_THREAD_ATTEMPT \success, .Lthread_retry2\@
.Lthread_retry2\@:
    LPR_THREAD_ATTEMPT \success, \failure
.endm
