/* SPDX-License-Identifier: MIT */
.text
.global gpud_ready_native_entry
.type gpud_ready_native_entry,@function
gpud_ready_native_entry:
 pop %rdi
 and $-16,%rsp
 call gpud_ready_worker_run
 mov $12,%eax
 xor %edi,%edi
 syscall
 ud2
.size gpud_ready_native_entry,.-gpud_ready_native_entry
.section .note.GNU-stack,"",@progbits
