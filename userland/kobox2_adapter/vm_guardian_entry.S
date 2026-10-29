/* SPDX-License-Identifier: MIT */
.text
.global _start
.type _start,@function
_start:
    and $-16, %rsp
    call ph_vm_guardian_main
    ud2
.size _start, .-_start
.section .note.GNU-stack,"",@progbits
