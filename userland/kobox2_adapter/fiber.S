/* SPDX-License-Identifier: MIT */
/* Hosted Linux task contexts. A switch happens only at a C call boundary
 * (Linux switch_to -> cpu_switch), so SysV caller-saved registers, including
 * every vector and x87 data register, are already dead. Asynchronous state at
 * a notification-handler switch lives in the native signal frame instead. */
/* One section per entry, so linkers can drop the ones a build does not use. */
.section .text.ph_fiber_switch,"ax",@progbits
.global ph_fiber_switch
.type ph_fiber_switch, @function
/* void ph_fiber_switch(uint64_t *save_rsp, uint64_t next_rsp) */
ph_fiber_switch:
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15
    /* MXCSR and the x87 control word are callee-saved control state. */
    sub $8, %rsp
    stmxcsr (%rsp)
    fnstcw 4(%rsp)
    mov %rsp, (%rdi)
    /* One instruction moves to the next stack: a notification delivered at
     * any point here finds a valid stack below RSP for its frame. */
    mov %rsi, %rsp
.global ph_fiber_resume_frame
ph_fiber_resume_frame:
    fldcw 4(%rsp)
    ldmxcsr (%rsp)
    add $8, %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx
    ret
.size ph_fiber_switch, . - ph_fiber_switch

.section .text.ph_fiber_start,"ax",@progbits
.global ph_fiber_start
.type ph_fiber_start, @function
/* First return target of a NEW fiber's frame; RBX holds the fiber and RSP is
 * 16-byte aligned here, as a call requires. */
ph_fiber_start:
    mov %rbx, %rdi
    call ph_fiber_main@PLT
    ud2
.size ph_fiber_start, . - ph_fiber_start

.section .text.ph_fiber_thread_entry,"ax",@progbits
.global ph_fiber_thread_entry
.type ph_fiber_thread_entry, @function
/* A native thread started directly on a NEW fiber: RSP is that fiber's
 * initial frame. Register this thread below the frame, then resume it. */
ph_fiber_thread_entry:
    call ph_fiber_thread_setup@PLT
    jmp ph_fiber_resume_frame
.size ph_fiber_thread_entry, . - ph_fiber_thread_entry

.section .text.ph_fault_stub,"ax",@progbits
.global ph_fault_stub
.type ph_fault_stub, @function
/* Linux exception handling resumes here on the faulting fiber's own stack,
 * after the native fault frame was returned. RDI is the fault record. */
ph_fault_stub:
    and $-16, %rsp
    call ph_fault_resume@PLT
    ud2
.size ph_fault_stub, . - ph_fault_stub
.section .note.GNU-stack,"",@progbits
