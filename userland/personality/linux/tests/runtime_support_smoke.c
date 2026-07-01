#include "../runtime/support/arena.h"
#include "../runtime/support/elf.h"
#include "../runtime/support/string.h"

#include <stdint.h>

static int expect(int condition)
{
    return condition ? 0 : 1;
}

static void wr16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value & 0xffu);
    p[1] = (uint8_t)((value >> 8) & 0xffu);
}

static void wr32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) {
        p[i] = (uint8_t)((value >> (i * 8u)) & 0xffu);
    }
}

static void wr64(uint8_t *p, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) {
        p[i] = (uint8_t)((value >> (i * 8u)) & 0xffu);
    }
}

static void put_phdr(uint8_t *p, uint32_t type, uint64_t off, uint64_t va, uint64_t filesz)
{
    wr32(p + 0, type);
    wr64(p + 8, off);
    wr64(p + 16, va);
    wr64(p + 24, va);
    wr64(p + 32, filesz);
    wr64(p + 40, filesz);
    wr64(p + 48, 4096);
}

static void put_dyn(uint8_t *p, uint64_t tag, uint64_t value)
{
    wr64(p, tag);
    wr64(p + 8, value);
}

int main(void)
{
    uint8_t memory[64];
    struct lpr_arena arena;
    lpr_arena_init(&arena, memory, sizeof(memory));
    char *copy = lpr_arena_strdup(&arena, "hello");
    if (expect(copy != 0 && lpr_strcmp(copy, "hello") == 0)) return 1;
    char *prefix = lpr_arena_strndup(&arena, "abcdef", 3);
    if (expect(prefix != 0 && lpr_strcmp(prefix, "abc") == 0)) return 1;
    void *aligned = lpr_arena_alloc(&arena, 8, 8);
    if (expect(aligned != 0 && (((uintptr_t)aligned & 7u) == 0))) return 1;
    if (expect(lpr_arena_alloc(&arena, 1024, 1) == 0)) return 1;

    if (expect(lpr_memchr("abc", 'b', 3) != 0)) return 1;
    if (expect(lpr_strnlen("abcdef", 3) == 3)) return 1;
    if (expect(lpr_strncmp("abc", "abd", 2) == 0)) return 1;
    if (expect(*lpr_strrchr("abca", 'a') == 'a')) return 1;

    uint8_t elf[1024];
    lpr_memset(elf, 0, sizeof(elf));
    elf[0] = 0x7f;
    elf[1] = 'E';
    elf[2] = 'L';
    elf[3] = 'F';
    elf[4] = LPR_ELF_CLASS_64;
    elf[5] = LPR_ELF_DATA_LSB;
    elf[6] = LPR_ELF_VERSION_CURRENT;
    wr16(elf + 16, LPR_ELF_TYPE_DYN);
    wr16(elf + 18, LPR_ELF_MACHINE_X86_64);
    wr32(elf + 20, LPR_ELF_VERSION_CURRENT);
    wr64(elf + 32, 64);
    wr16(elf + 52, LPR_ELF_EHDR_SIZE);
    wr16(elf + 54, LPR_ELF_PHDR_SIZE);
    wr16(elf + 56, 2);
    put_phdr(elf + 64, LPR_ELF_PT_LOAD, 0, 0, sizeof(elf));
    put_phdr(elf + 64 + LPR_ELF_PHDR_SIZE, LPR_ELF_PT_DYNAMIC, 0x200, 0x200, 5 * 16);
    put_dyn(elf + 0x200, LPR_ELF_DT_STRTAB, 0x300);
    put_dyn(elf + 0x210, LPR_ELF_DT_STRSZ, 32);
    put_dyn(elf + 0x220, LPR_ELF_DT_SYMTAB, 0x320);
    put_dyn(elf + 0x230, LPR_ELF_DT_NEEDED, 1);
    put_dyn(elf + 0x240, LPR_ELF_DT_NULL, 0);

    const struct lpr_elf_image image = {
        .bytes = elf,
        .size = sizeof(elf),
    };
    struct lpr_elf_dynamic_info dyn;
    if (expect(lpr_elf_validate64(&image) == 0)) return 1;
    if (expect(lpr_elf_scan_dynamic(&image, &dyn) == 0)) return 1;
    if (expect(dyn.dynamic_offset == 0x200 && dyn.dynamic_count == 5)) return 1;
    if (expect(dyn.strtab_va == 0x300 && dyn.strtab_offset == 0x300 && dyn.strsz == 32)) return 1;
    if (expect(dyn.symtab_va == 0x320 && dyn.symtab_offset == 0x320)) return 1;
    if (expect(dyn.needed_count == 1)) return 1;
    return 0;
}
