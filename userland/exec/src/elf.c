#include "exec_elf.h"

enum {
    ELF_HEADER_BYTES = 64,
    ELF_PHDR_BYTES = 56,
    ELF_MAGIC0 = 0x7f,
    ELF_MAGIC1 = 'E',
    ELF_MAGIC2 = 'L',
    ELF_MAGIC3 = 'F',
    ELF_CLASS_64 = 2,
    ELF_DATA_LSB = 1,
    ELF_VERSION_CURRENT = 1,
    ELF_TYPE_EXEC = EXEC_ELF_TYPE_EXEC,
    ELF_TYPE_DYN = EXEC_ELF_TYPE_DYN,
    ELF_MACHINE_X86_64 = 0x3e,
    PT_LOAD = EXEC_ELF_PT_LOAD,
    PT_DYNAMIC = EXEC_ELF_PT_DYNAMIC,
    PT_INTERP = EXEC_ELF_PT_INTERP,
    PT_TLS = EXEC_ELF_PT_TLS,
    PT_GNU_RELRO = EXEC_ELF_PT_GNU_RELRO,
    PF_X = EXEC_ELF_PF_X,
    PF_W = EXEC_ELF_PF_W,
    PF_R = EXEC_ELF_PF_R,
    PAGE_BYTES_LOCAL = EXEC_ELF_PAGE_BYTES,
};




int exec_elf_add_overflows_u64(exec_u64 a, exec_u64 b, exec_u64 *out) {
    *out = a + b;
    return *out < a;
}

static int mul_overflows_u64(exec_u64 a, exec_u64 b, exec_u64 *out) {
    if (a != 0 && b > (~0ULL / a)) return 1;
    *out = a * b;
    return 0;
}

exec_u64 exec_elf_page_down(exec_u64 value) {
    return value & ~(exec_u64)(PAGE_BYTES_LOCAL - 1);
}

int exec_elf_page_up(exec_u64 value, exec_u64 *out) {
    exec_u64 plus;
    if (exec_elf_add_overflows_u64(value, PAGE_BYTES_LOCAL - 1, &plus)) return 0;
    *out = exec_elf_page_down(plus);
    return 1;
}

static int is_power_of_two(exec_u64 value) {
    return value != 0 && (value & (value - 1)) == 0;
}

static exec_u16 read_u16_le(const unsigned char *bytes, exec_u64 off) {
    return (exec_u16)bytes[off] | ((exec_u16)bytes[off + 1] << 8);
}

static unsigned int read_u32_le(const unsigned char *bytes, exec_u64 off) {
    return (unsigned int)bytes[off] |
        ((unsigned int)bytes[off + 1] << 8) |
        ((unsigned int)bytes[off + 2] << 16) |
        ((unsigned int)bytes[off + 3] << 24);
}

static exec_u64 read_u64_le(const unsigned char *bytes, exec_u64 off) {
    exec_u64 value = 0;
    for (exec_u64 i = 0; i < 8; i++) value |= (exec_u64)bytes[off + i] << (i * 8);
    return value;
}

static int range_in_file(exec_u64 off, exec_u64 len, exec_u64 file_bytes) {
    exec_u64 end;
    if (exec_elf_add_overflows_u64(off, len, &end)) return 0;
    return end <= file_bytes;
}

enum exec_elf_error exec_elf_parse_header(const void *base, exec_u64 file_bytes, struct exec_elf_header *out) {
    const unsigned char *bytes = (const unsigned char *)base;
    if (file_bytes < ELF_HEADER_BYTES) return EXEC_ELF_ERR_SHORT;
    if (bytes[0] != ELF_MAGIC0 || bytes[1] != ELF_MAGIC1 || bytes[2] != ELF_MAGIC2 || bytes[3] != ELF_MAGIC3) return EXEC_ELF_ERR_MAGIC;
    if (bytes[4] != ELF_CLASS_64 || bytes[5] != ELF_DATA_LSB || bytes[6] != ELF_VERSION_CURRENT) return EXEC_ELF_ERR_CLASS;

    const exec_u16 elf_type = read_u16_le(bytes, 16);
    if (elf_type != ELF_TYPE_EXEC && elf_type != ELF_TYPE_DYN) return EXEC_ELF_ERR_UNSUPPORTED_TYPE;
    if (read_u16_le(bytes, 18) != ELF_MACHINE_X86_64) return EXEC_ELF_ERR_MACHINE;
    if (read_u32_le(bytes, 20) != ELF_VERSION_CURRENT) return EXEC_ELF_ERR_CLASS;

    out->elf_type = elf_type;
    out->entry = read_u64_le(bytes, 24);
    out->phoff = read_u64_le(bytes, 32);
    out->phentsize = read_u16_le(bytes, 54);
    out->phnum = read_u16_le(bytes, 56);
    return EXEC_ELF_OK;
}

enum exec_elf_error exec_elf_parse_program_header(
    const void *base,
    exec_u64 file_bytes,
    const struct exec_elf_header *ehdr,
    exec_u16 index,
    struct exec_elf_program_header *out
) {
    const unsigned char *bytes = (const unsigned char *)base;
    exec_u64 index_off;
    exec_u64 off;
    if (mul_overflows_u64((exec_u64)index, (exec_u64)ehdr->phentsize, &index_off)) return EXEC_ELF_ERR_PROGRAM_TABLE;
    if (exec_elf_add_overflows_u64(ehdr->phoff, index_off, &off)) return EXEC_ELF_ERR_PROGRAM_TABLE;
    if (!range_in_file(off, ELF_PHDR_BYTES, file_bytes)) return EXEC_ELF_ERR_PROGRAM_TABLE;

    out->p_type = read_u32_le(bytes, off + 0);
    out->flags = read_u32_le(bytes, off + 4);
    out->offset = read_u64_le(bytes, off + 8);
    out->vaddr = read_u64_le(bytes, off + 16);
    out->filesz = read_u64_le(bytes, off + 32);
    out->memsz = read_u64_le(bytes, off + 40);
    out->align_bytes = read_u64_le(bytes, off + 48);
    return EXEC_ELF_OK;
}

static int entry_inside_load(const struct exec_elf_program_header *phdr, exec_u64 entry) {
    exec_u64 end;
    if (exec_elf_add_overflows_u64(phdr->vaddr, phdr->memsz, &end)) return 0;
    return entry >= phdr->vaddr && entry < end;
}

enum exec_elf_error exec_elf_validate_load_segment(
    const struct exec_elf_program_header *phdr,
    exec_u64 file_bytes,
    exec_u64 *segment_start_out,
    exec_u64 *segment_end_out
) {
    if (phdr->memsz < phdr->filesz) return EXEC_ELF_ERR_SEGMENT;
    if (!range_in_file(phdr->offset, phdr->filesz, file_bytes)) return EXEC_ELF_ERR_SEGMENT;
    if (((phdr->offset ^ phdr->vaddr) & (PAGE_BYTES_LOCAL - 1)) != 0) return EXEC_ELF_ERR_SEGMENT;
    if (phdr->align_bytes > 1 && !is_power_of_two(phdr->align_bytes)) return EXEC_ELF_ERR_SEGMENT;

    const unsigned int known_flags = PF_X | PF_W | PF_R;
    if ((phdr->flags & ~known_flags) != 0) return EXEC_ELF_ERR_SEGMENT;
    if ((phdr->flags & PF_X) != 0 && (phdr->flags & PF_W) != 0) return EXEC_ELF_ERR_SEGMENT;

    const exec_u64 segment_start = exec_elf_page_down(phdr->vaddr);
    const exec_u64 page_delta = phdr->vaddr - segment_start;
    exec_u64 mem_end_delta;
    exec_u64 segment_bytes;
    exec_u64 segment_end;
    if (exec_elf_add_overflows_u64(page_delta, phdr->memsz, &mem_end_delta)) return EXEC_ELF_ERR_SEGMENT;
    if (!exec_elf_page_up(mem_end_delta, &segment_bytes)) return EXEC_ELF_ERR_SEGMENT;
    if (exec_elf_add_overflows_u64(segment_start, segment_bytes, &segment_end)) return EXEC_ELF_ERR_SEGMENT;
    if (segment_end <= segment_start) return EXEC_ELF_ERR_SEGMENT;

    *segment_start_out = segment_start;
    *segment_end_out = segment_end;
    return EXEC_ELF_OK;
}

enum exec_elf_error exec_elf_validate_image(const void *base, exec_u64 file_bytes, struct exec_elf_summary *out) {
    const unsigned char *bytes = (const unsigned char *)base;
    struct exec_elf_summary summary;
    for (exec_u64 i = 0; i < sizeof(summary); i++) ((unsigned char *)&summary)[i] = 0;

    enum exec_elf_error err = exec_elf_parse_header(bytes, file_bytes, &summary.header);
    if (err != EXEC_ELF_OK) return err;
    if (summary.header.phentsize < ELF_PHDR_BYTES || summary.header.phnum == 0 || summary.header.phnum > EXEC_ELF_MAX_PHNUM) return EXEC_ELF_ERR_PROGRAM_TABLE;

    exec_u64 phdr_bytes;
    if (mul_overflows_u64((exec_u64)summary.header.phnum, (exec_u64)summary.header.phentsize, &phdr_bytes)) return EXEC_ELF_ERR_PROGRAM_TABLE;
    if (!range_in_file(summary.header.phoff, phdr_bytes, file_bytes)) return EXEC_ELF_ERR_PROGRAM_TABLE;

    int have_load = 0;
    int entry_ok = 0;
    summary.max_align = PAGE_BYTES_LOCAL;
    summary.is_pie = summary.header.elf_type == ELF_TYPE_DYN;

    for (exec_u16 i = 0; i < summary.header.phnum; i++) {
        struct exec_elf_program_header phdr;
        err = exec_elf_parse_program_header(bytes, file_bytes, &summary.header, i, &phdr);
        if (err != EXEC_ELF_OK) return err;

        if (phdr.p_type == PT_INTERP) {
            if (!range_in_file(phdr.offset, phdr.filesz, file_bytes) || phdr.filesz == 0 || phdr.memsz < phdr.filesz) return EXEC_ELF_ERR_SEGMENT;
            summary.has_interp = 1;
        } else if (phdr.p_type == PT_DYNAMIC) {
            if (!range_in_file(phdr.offset, phdr.filesz, file_bytes) || (phdr.filesz % 16) != 0) return EXEC_ELF_ERR_SEGMENT;
            summary.has_dynamic = 1;
        } else if (phdr.p_type == PT_TLS) {
            summary.has_tls = 1;
        } else if (phdr.p_type == PT_GNU_RELRO) {
            summary.has_relro = 1;
        }

        if (phdr.p_type != PT_LOAD) continue;
        exec_u64 segment_start;
        exec_u64 segment_end;
        err = exec_elf_validate_load_segment(&phdr, file_bytes, &segment_start, &segment_end);
        if (err != EXEC_ELF_OK) return err;
        if (!have_load || segment_start < summary.min_load_vaddr) summary.min_load_vaddr = segment_start;
        if (!have_load || segment_end > summary.max_load_vaddr) summary.max_load_vaddr = segment_end;
        if (phdr.align_bytes > summary.max_align) summary.max_align = phdr.align_bytes;
        if (entry_inside_load(&phdr, summary.header.entry)) entry_ok = 1;
        have_load = 1;
    }

    if (!have_load || summary.max_load_vaddr <= summary.min_load_vaddr) return EXEC_ELF_ERR_NO_LOAD;
    if (!entry_ok) return EXEC_ELF_ERR_ENTRY;
    if (out != 0) *out = summary;
    return EXEC_ELF_OK;
}

const char *exec_elf_error_name(enum exec_elf_error error) {
    switch (error) {
        case EXEC_ELF_OK: return "ok";
        case EXEC_ELF_ERR_SHORT: return "short";
        case EXEC_ELF_ERR_MAGIC: return "magic";
        case EXEC_ELF_ERR_CLASS: return "class";
        case EXEC_ELF_ERR_MACHINE: return "machine";
        case EXEC_ELF_ERR_UNSUPPORTED_TYPE: return "type";
        case EXEC_ELF_ERR_PROGRAM_TABLE: return "phdr";
        case EXEC_ELF_ERR_SEGMENT: return "segment";
        case EXEC_ELF_ERR_NO_LOAD: return "no_load";
        case EXEC_ELF_ERR_ENTRY: return "entry";
    }
    return "unknown";
}
