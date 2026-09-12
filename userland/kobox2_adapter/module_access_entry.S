/* SPDX-License-Identifier: MIT */
.text
.global ph_module_fault_entry
.type ph_module_fault_entry,@function
ph_module_fault_entry:
    call ph_module_access_fault
    ud2
.size ph_module_fault_entry, .-ph_module_fault_entry

.global ph_module_probe
.type ph_module_probe,@function
ph_module_probe:
    cmp $2, %esi
    je .Lexecute
.global ph_module_probe_read
ph_module_probe_read:
    mov (%rdi), %rax
    cmp $1, %esi
    jne .Ldone
.global ph_module_probe_write
ph_module_probe_write:
    mov %rax, (%rdi)
.Ldone:
    ret
.Lexecute:
    sub $8, %rsp
    movabs $0x6b62326d6f64756c, %r11
    call *%rdi
    add $8, %rsp
    ret
.size ph_module_probe, .-ph_module_probe
.section .note.GNU-stack,"",@progbits
