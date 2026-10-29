/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <elf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <patch_scan.h>

int lpr_exec_patch_syscalls(unsigned char *, uint64_t, uint64_t *);

int main(int argc, char **argv)
{
    unsigned char code[320], expected[320];
    memset(code, 0x90, sizeof(code));
    code[0] = 0x06; /* Invalid as a long-mode instruction; inline assembler data. */
    code[300] = 0xb8;
    code[301] = 39;
    code[302] = code[303] = code[304] = 0;
    code[305] = 0x0f;
    code[306] = 0x05;
    code[307] = 0xc3;
    memcpy(expected, code, sizeof(code));
    lpr_patch_scan_result_t scan;
    assert(lpr_patch_scan_syscalls(expected, sizeof(expected), 0, &scan) == 0);
    uint64_t patched = 999;
    assert(lpr_exec_patch_syscalls(code, sizeof(code), &patched) == 0);
    assert(patched == 1 && !memcmp(code, expected, sizeof(code)));

    unsigned char ambiguous[] = {0x06, 0x0f, 0x05, 0xc3};
    unsigned char original[sizeof(ambiguous)];
    memcpy(original, ambiguous, sizeof(original));
    assert(lpr_exec_patch_syscalls(ambiguous, sizeof(ambiguous), &patched) == -8);
    assert(patched == 0 && !memcmp(ambiguous, original, sizeof(original)));

    for (int arg = 1; arg < argc; ++arg) {
        FILE *file = fopen(argv[arg], "rb");
        assert(file && !fseek(file, 0, SEEK_END));
        const long length = ftell(file);
        assert(length >= (long)sizeof(Elf64_Ehdr) && !fseek(file, 0, SEEK_SET));
        unsigned char *image = malloc((size_t)length);
        assert(image && fread(image, 1, (size_t)length, file) == (size_t)length);
        fclose(file);
        const Elf64_Ehdr *eh = (const void *)image;
        assert(!memcmp(eh->e_ident, ELFMAG, SELFMAG));
        assert(eh->e_shentsize == sizeof(Elf64_Shdr) && eh->e_shoff <= (uint64_t)length);
        assert(eh->e_shnum <= ((uint64_t)length - eh->e_shoff) / sizeof(Elf64_Shdr));
        const Elf64_Shdr *sections = (const void *)(image + eh->e_shoff);
        unsigned tested = 0;
        for (unsigned i = 0; i < eh->e_shnum; ++i) {
            const Elf64_Shdr *section = &sections[i];
            if (!(section->sh_flags & SHF_EXECINSTR) || !section->sh_size) continue;
            assert(section->sh_offset <= (uint64_t)length &&
                section->sh_size <= (uint64_t)length - section->sh_offset);
            unsigned char *actual = malloc(section->sh_size);
            unsigned char *oracle = malloc(section->sh_size);
            assert(actual && oracle);
            memcpy(actual, image + section->sh_offset, section->sh_size);
            memcpy(oracle, actual, section->sh_size);
            assert(lpr_patch_scan_syscalls(oracle, section->sh_size, 0, &scan) == 0);
            assert(lpr_exec_patch_syscalls(actual, section->sh_size, &patched) == 0);
            assert(patched == scan.patched_sites && !memcmp(actual, oracle, section->sh_size));
            printf("exec section %u: %llu syscall sites PASS\n", i, (unsigned long long)patched);
            ++tested;
            free(actual);
            free(oracle);
        }
        assert(tested);
        free(image);
    }
    puts("FileD exec scan: inline data fallback, ambiguous rejection and failure atomicity PASS");
}
