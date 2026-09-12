/* SPDX-License-Identifier: MIT */
/* Static PIE entry for running the native contracts on a boot-only image. */
#include <elf.h>
#include <stdint.h>
#include <stddef.h>
#include <pacha/syscall.h>
#include <pacha/abi.h>

extern int main(void);
void native_test_start(uintptr_t base) {
    const Elf64_Ehdr *eh = (void *)base;
    const Elf64_Phdr *ph = (void *)(base + eh->e_phoff);
    for (unsigned i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != PT_DYNAMIC) continue;
        const Elf64_Dyn *dyn = (void *)(base + ph[i].p_vaddr);
        Elf64_Rela *rela = NULL; size_t bytes = 0;
        for (; dyn->d_tag; ++dyn) {
            if (dyn->d_tag == DT_RELA) rela = (void *)(base + dyn->d_un.d_ptr);
            if (dyn->d_tag == DT_RELASZ) bytes = dyn->d_un.d_val;
        }
        for (size_t j = 0; j < bytes / sizeof(*rela); ++j) {
            if (ELF64_R_TYPE(rela[j].r_info) != R_X86_64_RELATIVE) __builtin_trap();
            *(uintptr_t *)(base + rela[j].r_offset) = base + rela[j].r_addend;
        }
    }
    int status = main();
    (void)pacha_syscall1(PACHA_PROCESS_SYSCALL_EXIT, status);
    __builtin_trap();
}
__asm__(".text\n.global _start\n_start: and $-16, %rsp; "
    "lea __ehdr_start(%rip), %rdi; call native_test_start; ud2\n"
    ".section .note.GNU-stack,\"\",@progbits\n");
