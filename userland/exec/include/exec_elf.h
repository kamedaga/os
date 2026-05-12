#ifndef CAPABILITYOS_EXEC_ELF_H
#define CAPABILITYOS_EXEC_ELF_H

#include "exec_abi.h"

#define EXEC_ELF_MAX_PHNUM 128
#define EXEC_ELF_PAGE_BYTES 4096ULL
#define EXEC_ELF_TYPE_EXEC 2
#define EXEC_ELF_TYPE_DYN 3
#define EXEC_ELF_PT_LOAD 1U
#define EXEC_ELF_PT_DYNAMIC 2U
#define EXEC_ELF_PT_INTERP 3U
#define EXEC_ELF_PT_TLS 7U
#define EXEC_ELF_PT_GNU_RELRO 0x6474e552U
#define EXEC_ELF_PF_X (1U << 0)
#define EXEC_ELF_PF_W (1U << 1)
#define EXEC_ELF_PF_R (1U << 2)

enum exec_elf_error {
    EXEC_ELF_OK = 0,
    EXEC_ELF_ERR_SHORT = 1,
    EXEC_ELF_ERR_MAGIC = 2,
    EXEC_ELF_ERR_CLASS = 3,
    EXEC_ELF_ERR_MACHINE = 4,
    EXEC_ELF_ERR_UNSUPPORTED_TYPE = 5,
    EXEC_ELF_ERR_PROGRAM_TABLE = 6,
    EXEC_ELF_ERR_SEGMENT = 7,
    EXEC_ELF_ERR_NO_LOAD = 8,
    EXEC_ELF_ERR_ENTRY = 9,
};

struct exec_elf_header {
    exec_u16 elf_type;
    exec_u16 phentsize;
    exec_u16 phnum;
    exec_u64 entry;
    exec_u64 phoff;
};

struct exec_elf_program_header {
    unsigned int p_type;
    unsigned int flags;
    exec_u64 offset;
    exec_u64 vaddr;
    exec_u64 filesz;
    exec_u64 memsz;
    exec_u64 align_bytes;
};

struct exec_elf_summary {
    struct exec_elf_header header;
    exec_u64 min_load_vaddr;
    exec_u64 max_load_vaddr;
    exec_u64 max_align;
    int has_interp;
    int has_dynamic;
    int has_tls;
    int has_relro;
    int is_pie;
};

enum exec_elf_error exec_elf_validate_image(const void *base, exec_u64 file_bytes, struct exec_elf_summary *out);
enum exec_elf_error exec_elf_parse_header(const void *base, exec_u64 file_bytes, struct exec_elf_header *out);
enum exec_elf_error exec_elf_parse_program_header(const void *base, exec_u64 file_bytes, const struct exec_elf_header *ehdr, exec_u16 index, struct exec_elf_program_header *out);
enum exec_elf_error exec_elf_validate_load_segment(const struct exec_elf_program_header *phdr, exec_u64 file_bytes, exec_u64 *segment_start_out, exec_u64 *segment_end_out);
int exec_elf_add_overflows_u64(exec_u64 a, exec_u64 b, exec_u64 *out);
exec_u64 exec_elf_page_down(exec_u64 value);
int exec_elf_page_up(exec_u64 value, exec_u64 *out);
const char *exec_elf_error_name(enum exec_elf_error error);

#endif
