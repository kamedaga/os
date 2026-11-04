/* Entered only for Linux clock_gettime(CLOCK_MONOTONIC). No C call and no
 * FP/SIMD instruction: the two transit words are the SAME maximum caller-stack
 * use as ordinary entry, including on small Go goroutine stacks. All Linux
 * argument/callee registers survive. A miss restores them before normal entry.
 *
 * This leaf accepts only an already published single-thread signal slot with
 * no queued/native signal. Count transitions invalidate that slot before reuse;
 * thread_count is checked separately, so a stale publication cannot authorize
 * borrowing another thread's identity. Every other case keeps full dispatch.
 */
.hidden lpr_clock_syscall_leaf
.globl lpr_clock_syscall_leaf
.type lpr_clock_syscall_leaf,@function
lpr_clock_syscall_leaf:
    cmp $228, %rax
    jne lpr_syscall_entry_slow
    cmp $1, %rdi
    jne lpr_syscall_entry_slow
    push %rcx
    push %rdx
    mov lpr_clock_thread_count(%rip), %r11
    cmpl $1, (%r11)
    jne .Lclock_leaf_slow
    mov lpr_single_thread_signal_state(%rip), %r11
    test %r11, %r11
    jz .Lclock_leaf_slow
    cmpl $1, LPR_CLOCK_HINT_ACTIVE_OFFSET(%r11)
    jne .Lclock_leaf_slow
    cmpl $0, LPR_CLOCK_HINT_OFFSET(%r11)
    jne .Lclock_leaf_slow
    cmpq $0, LPR_CLOCK_PENDING_OFFSET(%r11)
    jne .Lclock_leaf_slow
    mov lpr_clock_fast_page(%rip), %r11
    test %r11, %r11
    jz .Lclock_leaf_slow
    cmpl $1, 0(%r11)
    jne .Lclock_leaf_slow
    /* An immutable synthetic record starts at seqlock=2. Mutable KVM records
     * may also use this leaf while both checks see 2; any other even value
     * simply uses the general reader. This is a conservative optimization,
     * not a new ABI version or a weakening of the snapshot check. */
    cmpl $2, 64(%r11)
    jne .Lclock_leaf_slow
    testb $1, 93(%r11)
    jz .Lclock_leaf_slow
    movsbl 92(%r11), %ecx
    test %ecx, %ecx
    jg .Lclock_leaf_slow
    cmp $-63, %ecx
    jl .Lclock_leaf_slow
#ifdef LPR_CLOCK_ENTRY_TEST
    mov lpr_clock_test_counter(%rip), %rax
    /* Deterministic publication-race injection, absent from production. */
    cmpl $1, lpr_clock_test_mutation(%rip)
    jne .Lclock_test_fast_mutation
    movl $4, 64(%r11)
.Lclock_test_fast_mutation:
    cmpl $2, lpr_clock_test_mutation(%rip)
    jne .Lclock_test_counter_ready
    movl $0, 0(%r11)
.Lclock_test_counter_ready:
#else
    lfence
    rdtsc
    lfence
    shl $32, %rdx
    or %rdx, %rax
#endif
    sub 72(%r11), %rax
    jc .Lclock_leaf_slow
    neg %ecx
    shr %cl, %rax
    mov 88(%r11), %ecx
    test %ecx, %ecx
    jz .Lclock_leaf_slow
    mul %rcx
    shrd $32, %rdx, %rax
    shr $32, %rdx
    jnz .Lclock_leaf_slow
    add 80(%r11), %rax
    jc .Lclock_leaf_slow
    sub 8(%r11), %rax
    jc .Lclock_leaf_slow
    add 16(%r11), %rax
    jc .Lclock_leaf_slow
    cmp 32(%r11), %rax
    ja .Lclock_leaf_slow
    cmp 24(%r11), %rax
    cmovb 24(%r11), %rax
    /* A concurrent native publisher can advance floor after the first
     * ceiling read. The returned value must itself still fit a live lease. */
    cmp 32(%r11), %rax
    ja .Lclock_leaf_slow
    cmpl $2, 64(%r11)
    jne .Lclock_leaf_slow
    cmpl $1, 0(%r11)
    jne .Lclock_leaf_slow
    mov lpr_clock_thread_count(%rip), %rdx
    cmpl $1, (%rdx)
    jne .Lclock_leaf_slow
    mov lpr_single_thread_signal_state(%rip), %rdx
    test %rdx, %rdx
    jz .Lclock_leaf_slow
    cmpl $1, LPR_CLOCK_HINT_ACTIVE_OFFSET(%rdx)
    jne .Lclock_leaf_slow
    cmpl $0, LPR_CLOCK_HINT_OFFSET(%rdx)
    jne .Lclock_leaf_slow
    cmpq $0, LPR_CLOCK_PENDING_OFFSET(%rdx)
    jne .Lclock_leaf_slow
    /* Match lpr_user_range_plausible for the entire 16-byte output. Only the
     * two stores below may recover a write fault; native COW still resolves
     * normally before the registered trap handler considers EFAULT. */
    cmp $0x100000, %rsi
    jb lpr_clock_entry_copy_bad
    mov %rsi, %rcx
    add $15, %rcx
    jc lpr_clock_entry_copy_bad
    shr $47, %rcx
    jnz lpr_clock_entry_copy_bad
    xor %edx, %edx
    mov $1000000000, %ecx
    div %rcx
.globl lpr_clock_entry_copy_seconds
.hidden lpr_clock_entry_copy_seconds
lpr_clock_entry_copy_seconds:
    mov %rax, (%rsi)
.globl lpr_clock_entry_copy_nanos
.hidden lpr_clock_entry_copy_nanos
lpr_clock_entry_copy_nanos:
    mov %rdx, 8(%rsi)
    xor %eax, %eax
.Lclock_leaf_return:
    pop %rdx
    pop %rcx
    jmp *%rcx
.globl lpr_clock_entry_copy_bad
.hidden lpr_clock_entry_copy_bad
lpr_clock_entry_copy_bad:
    mov $-14, %rax
    jmp .Lclock_leaf_return
.Lclock_leaf_slow:
    pop %rdx
    pop %rcx
    mov $228, %eax
    jmp lpr_syscall_entry_slow
.size lpr_clock_syscall_leaf,.-lpr_clock_syscall_leaf
