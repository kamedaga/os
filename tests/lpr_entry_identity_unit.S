.text
.globl lpr_entry_test_gettid
lpr_entry_test_gettid:
    incl entry_test_tid_calls(%rip)
    mov entry_test_tid(%rip),%rax
    ret
.globl entry_test_call
entry_test_call:
    mov $39,%eax
    mov $555,%edx
    mov $123,%edi
    lea .Lentry_return(%rip),%rcx
    jmp lpr_syscall_entry
.Lentry_return:ret
.Lunexpected:ud2
.globl lpr_clone_thread_bootstrap,lpr_fork_child_bootstrap,lpr_linux_async_signal_prepare,lpr_linux_fault_prepare,lpr_linux_rt_sigreturn_body
.set lpr_clone_thread_bootstrap,.Lunexpected
.set lpr_fork_child_bootstrap,.Lunexpected
.set lpr_linux_async_signal_prepare,.Lunexpected
.set lpr_linux_fault_prepare,.Lunexpected
.set lpr_linux_rt_sigreturn_body,.Lunexpected
.section .note.GNU-stack,"",@progbits
