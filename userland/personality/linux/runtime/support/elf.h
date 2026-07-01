#ifndef LPR_SUPPORT_ELF_H
#define LPR_SUPPORT_ELF_H

#include <stdint.h>

enum {
    LPR_ELF_EHDR_SIZE = 64,
    LPR_ELF_PHDR_SIZE = 56,
    LPR_ELF_CLASS_64 = 2,
    LPR_ELF_DATA_LSB = 1,
    LPR_ELF_VERSION_CURRENT = 1,
    LPR_ELF_TYPE_EXEC = 2,
    LPR_ELF_TYPE_DYN = 3,
    LPR_ELF_MACHINE_X86_64 = 0x3e,
    LPR_ELF_PT_LOAD = 1,
    LPR_ELF_PT_DYNAMIC = 2,
    LPR_ELF_DT_NULL = 0,
    LPR_ELF_DT_NEEDED = 1,
    LPR_ELF_DT_PLTRELSZ = 2,
    LPR_ELF_DT_STRTAB = 5,
    LPR_ELF_DT_SYMTAB = 6,
    LPR_ELF_DT_RELA = 7,
    LPR_ELF_DT_RELASZ = 8,
    LPR_ELF_DT_RELAENT = 9,
    LPR_ELF_DT_STRSZ = 10,
    LPR_ELF_DT_INIT = 12,
    LPR_ELF_DT_FINI = 13,
    LPR_ELF_DT_SONAME = 14,
    LPR_ELF_DT_RPATH = 15,
    LPR_ELF_DT_SYMBOLIC = 16,
    LPR_ELF_DT_REL = 17,
    LPR_ELF_DT_RELSZ = 18,
    LPR_ELF_DT_RELENT = 19,
    LPR_ELF_DT_PLTREL = 20,
    LPR_ELF_DT_DEBUG = 21,
    LPR_ELF_DT_TEXTREL = 22,
    LPR_ELF_DT_JMPREL = 23,
    LPR_ELF_DT_BIND_NOW = 24,
    LPR_ELF_DT_INIT_ARRAY = 25,
    LPR_ELF_DT_FINI_ARRAY = 26,
    LPR_ELF_DT_INIT_ARRAYSZ = 27,
    LPR_ELF_DT_FINI_ARRAYSZ = 28,
    LPR_ELF_DT_RUNPATH = 29,
    LPR_ELF_DT_FLAGS = 30,
    LPR_ELF_DT_GNU_HASH = 0x6ffffef5,
    LPR_ELF_DT_FLAGS_1 = 0x6ffffffb,
    LPR_ELF_DT_VERNEED = 0x6ffffffe,
    LPR_ELF_DT_VERNEEDNUM = 0x6fffffff,
};

struct lpr_elf_image {
    const uint8_t *bytes;
    uint64_t size;
};

struct lpr_elf_dynamic_info {
    uint64_t dynamic_offset;
    uint64_t dynamic_count;
    uint64_t strtab_va;
    uint64_t strtab_offset;
    uint64_t strsz;
    uint64_t symtab_va;
    uint64_t symtab_offset;
    uint64_t gnu_hash_va;
    uint64_t gnu_hash_offset;
    uint64_t rela_va;
    uint64_t rela_offset;
    uint64_t rela_size;
    uint64_t rela_ent;
    uint64_t jmprel_va;
    uint64_t jmprel_offset;
    uint64_t pltrel_size;
    uint64_t init_array_va;
    uint64_t init_array_offset;
    uint64_t init_array_size;
    uint64_t flags;
    uint64_t flags_1;
    uint32_t needed_count;
};

int lpr_elf_validate64(const struct lpr_elf_image *image);
int lpr_elf_vaddr_to_offset(const struct lpr_elf_image *image, uint64_t vaddr, uint64_t size, uint64_t *out_offset);
int lpr_elf_scan_dynamic(const struct lpr_elf_image *image, struct lpr_elf_dynamic_info *out);

#endif
