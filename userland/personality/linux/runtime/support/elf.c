#include "elf.h"
#include "string.h"

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] |
        ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) |
        ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static int range_in_image(const struct lpr_elf_image *image, uint64_t offset, uint64_t size)
{
    return image != 0 && image->bytes != 0 && offset <= image->size && size <= image->size - offset;
}

static int add_overflow(uint64_t a, uint64_t b, uint64_t *out)
{
    if (out == 0 || a > UINT64_MAX - b) {
        return 1;
    }
    *out = a + b;
    return 0;
}

int lpr_elf_validate64(const struct lpr_elf_image *image)
{
    if (!range_in_image(image, 0, LPR_ELF_EHDR_SIZE)) {
        return -1;
    }
    const uint8_t *e = image->bytes;
    if (e[0] != 0x7f || e[1] != 'E' || e[2] != 'L' || e[3] != 'F') {
        return -1;
    }
    if (e[4] != LPR_ELF_CLASS_64 ||
        e[5] != LPR_ELF_DATA_LSB ||
        e[6] != LPR_ELF_VERSION_CURRENT)
    {
        return -1;
    }
    const uint16_t type = rd16(e + 16);
    const uint16_t machine = rd16(e + 18);
    const uint32_t version = rd32(e + 20);
    const uint64_t phoff = rd64(e + 32);
    const uint16_t phentsize = rd16(e + 54);
    const uint16_t phnum = rd16(e + 56);
    if ((type != LPR_ELF_TYPE_EXEC && type != LPR_ELF_TYPE_DYN) ||
        machine != LPR_ELF_MACHINE_X86_64 ||
        version != LPR_ELF_VERSION_CURRENT ||
        phentsize < LPR_ELF_PHDR_SIZE ||
        phnum == 0)
    {
        return -1;
    }
    if (phoff > image->size || (uint64_t)phentsize * phnum > image->size - phoff) {
        return -1;
    }
    return 0;
}

int lpr_elf_vaddr_to_offset(const struct lpr_elf_image *image, uint64_t vaddr, uint64_t size, uint64_t *out_offset)
{
    if (out_offset == 0 || lpr_elf_validate64(image) != 0) {
        return -1;
    }
    *out_offset = 0;
    const uint8_t *e = image->bytes;
    const uint64_t phoff = rd64(e + 32);
    const uint16_t phentsize = rd16(e + 54);
    const uint16_t phnum = rd16(e + 56);
    for (uint16_t i = 0; i < phnum; ++i) {
        const uint8_t *ph = image->bytes + phoff + (uint64_t)i * phentsize;
        if (rd32(ph) != LPR_ELF_PT_LOAD) {
            continue;
        }
        const uint64_t p_offset = rd64(ph + 8);
        const uint64_t p_vaddr = rd64(ph + 16);
        const uint64_t p_filesz = rd64(ph + 32);
        uint64_t vend;
        uint64_t want_end;
        if (add_overflow(p_vaddr, p_filesz, &vend) != 0 ||
            add_overflow(vaddr, size, &want_end) != 0)
        {
            return -1;
        }
        if (vaddr < p_vaddr || want_end > vend) {
            continue;
        }
        const uint64_t delta = vaddr - p_vaddr;
        uint64_t offset;
        if (add_overflow(p_offset, delta, &offset) != 0 || !range_in_image(image, offset, size)) {
            return -1;
        }
        *out_offset = offset;
        return 0;
    }
    return -1;
}

static void remember_ptr(const struct lpr_elf_image *image, uint64_t va, uint64_t size, uint64_t *out_va, uint64_t *out_offset)
{
    uint64_t offset;
    *out_va = va;
    *out_offset = lpr_elf_vaddr_to_offset(image, va, size, &offset) == 0 ? offset : 0;
}

int lpr_elf_scan_dynamic(const struct lpr_elf_image *image, struct lpr_elf_dynamic_info *out)
{
    if (out == 0 || lpr_elf_validate64(image) != 0) {
        return -1;
    }
    lpr_memset(out, 0, sizeof(*out));
    const uint8_t *e = image->bytes;
    const uint64_t phoff = rd64(e + 32);
    const uint16_t phentsize = rd16(e + 54);
    const uint16_t phnum = rd16(e + 56);
    for (uint16_t i = 0; i < phnum; ++i) {
        const uint8_t *ph = image->bytes + phoff + (uint64_t)i * phentsize;
        if (rd32(ph) != LPR_ELF_PT_DYNAMIC) {
            continue;
        }
        const uint64_t p_offset = rd64(ph + 8);
        const uint64_t p_filesz = rd64(ph + 32);
        if (!range_in_image(image, p_offset, p_filesz) || (p_filesz % 16u) != 0) {
            return -1;
        }
        out->dynamic_offset = p_offset;
        out->dynamic_count = p_filesz / 16u;
        for (uint64_t j = 0; j < out->dynamic_count; ++j) {
            const uint8_t *dyn = image->bytes + p_offset + j * 16u;
            const uint64_t tag = rd64(dyn);
            const uint64_t val = rd64(dyn + 8);
            if (tag == LPR_ELF_DT_NULL) {
                return 0;
            }
            switch (tag) {
            case LPR_ELF_DT_NEEDED:
                out->needed_count++;
                break;
            case LPR_ELF_DT_STRTAB:
                remember_ptr(image, val, 0, &out->strtab_va, &out->strtab_offset);
                break;
            case LPR_ELF_DT_STRSZ:
                out->strsz = val;
                break;
            case LPR_ELF_DT_SYMTAB:
                remember_ptr(image, val, 0, &out->symtab_va, &out->symtab_offset);
                break;
            case LPR_ELF_DT_GNU_HASH:
                remember_ptr(image, val, 0, &out->gnu_hash_va, &out->gnu_hash_offset);
                break;
            case LPR_ELF_DT_RELA:
                remember_ptr(image, val, 0, &out->rela_va, &out->rela_offset);
                break;
            case LPR_ELF_DT_RELASZ:
                out->rela_size = val;
                break;
            case LPR_ELF_DT_RELAENT:
                out->rela_ent = val;
                break;
            case LPR_ELF_DT_JMPREL:
                remember_ptr(image, val, 0, &out->jmprel_va, &out->jmprel_offset);
                break;
            case LPR_ELF_DT_PLTRELSZ:
                out->pltrel_size = val;
                break;
            case LPR_ELF_DT_INIT_ARRAY:
                remember_ptr(image, val, 0, &out->init_array_va, &out->init_array_offset);
                break;
            case LPR_ELF_DT_INIT_ARRAYSZ:
                out->init_array_size = val;
                break;
            case LPR_ELF_DT_FLAGS:
                out->flags = val;
                break;
            case LPR_ELF_DT_FLAGS_1:
                out->flags_1 = val;
                break;
            default:
                break;
            }
        }
        return 0;
    }
    return 0;
}
