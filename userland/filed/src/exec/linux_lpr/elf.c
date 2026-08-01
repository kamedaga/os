#include "internal.h"

#include <patch_scan.h>
#include <string.h>

int lpr_exec_validate_elf(const lpr_exec_image_t *image)
{
    if (image == NULL || image->bytes == NULL || image->size < LPR_EXEC_EHDR_BYTES) {
        return -22;
    }
    const unsigned char *bytes = image->bytes;
    if (bytes[0] != 0x7f || bytes[1] != 'E' || bytes[2] != 'L' || bytes[3] != 'F') {
        return -8;
    }
    if (bytes[4] != LPR_EXEC_ELF_CLASS_64 ||
        bytes[5] != LPR_EXEC_ELF_DATA_LSB ||
        bytes[6] != LPR_EXEC_ELF_VERSION_CURRENT)
    {
        return -8;
    }
    const uint16_t e_type = lpr_exec_rd16(bytes + 16);
    const uint16_t e_machine = lpr_exec_rd16(bytes + 18);
    const uint32_t e_version = lpr_exec_rd32(bytes + 20);
    const uint64_t e_phoff = lpr_exec_rd64(bytes + 32);
    const uint16_t e_phentsize = lpr_exec_rd16(bytes + 54);
    const uint16_t e_phnum = lpr_exec_rd16(bytes + 56);
    if ((e_type != LPR_EXEC_ELF_TYPE_EXEC && e_type != LPR_EXEC_ELF_TYPE_DYN) ||
        e_machine != LPR_EXEC_ELF_MACHINE_X86_64 ||
        e_version != LPR_EXEC_ELF_VERSION_CURRENT ||
        e_phentsize < LPR_EXEC_PHDR_BYTES ||
        e_phnum == 0)
    {
        return -8;
    }
    if (e_phoff > image->size || (uint64_t)e_phentsize * e_phnum > image->size - e_phoff) {
        return -8;
    }
    return 0;
}

int lpr_exec_get_interp_path(const lpr_exec_image_t *image, char *out_path, size_t out_size)
{
    if (out_path == NULL || out_size == 0) {
        return -22;
    }
    out_path[0] = '\0';
    int status = lpr_exec_validate_elf(image);
    if (status != 0) {
        return status;
    }
    const unsigned char *bytes = image->bytes;
    const uint64_t e_phoff = lpr_exec_rd64(bytes + 32);
    const uint16_t e_phentsize = lpr_exec_rd16(bytes + 54);
    const uint16_t e_phnum = lpr_exec_rd16(bytes + 56);
    for (uint16_t i = 0; i < e_phnum; ++i) {
        const unsigned char *ph = bytes + e_phoff + (uint64_t)i * e_phentsize;
        if (lpr_exec_rd32(ph) != LPR_EXEC_PT_INTERP) {
            continue;
        }
        const uint64_t p_offset = lpr_exec_rd64(ph + 8);
        const uint64_t p_filesz = lpr_exec_rd64(ph + 32);
        if (out_path[0] != '\0' ||
            p_filesz == 0 ||
            p_filesz >= out_size ||
            p_filesz > LPR_EXEC_MAX_INTERP_BYTES ||
            p_offset > image->size ||
            p_filesz > image->size - p_offset ||
            image->bytes[p_offset + p_filesz - 1u] != '\0')
        {
            return -8;
        }
        memcpy(out_path, image->bytes + p_offset, (size_t)p_filesz);
        if (out_path[0] != '/') {
            return -8;
        }
    }
    return 0;
}

int lpr_exec_meta_get_interp_path(
    filed_runtime_t *runtime,
    const lpr_exec_file_t *file,
    const lpr_exec_meta_t *meta,
    char *out_path,
    size_t out_size)
{
    if (out_path == NULL || out_size == 0) {
        return -22;
    }
    out_path[0] = '\0';
    if (runtime == NULL || file == NULL || meta == NULL || meta->phdrs == NULL) {
        return -22;
    }
    if (meta->interp_path[0] != '\0') {
        const size_t cached_len = strnlen(meta->interp_path, sizeof(meta->interp_path));
        if (cached_len == 0 || cached_len >= out_size || cached_len >= sizeof(meta->interp_path)) {
            return -8;
        }
        memcpy(out_path, meta->interp_path, cached_len + 1u);
        return 0;
    }
    for (uint16_t i = 0; i < meta->phnum; ++i) {
        const unsigned char *ph = meta->phdrs + (uint64_t)i * meta->phent;
        if (lpr_exec_rd32(ph) != LPR_EXEC_PT_INTERP) {
            continue;
        }
        const uint64_t p_offset = lpr_exec_rd64(ph + 8);
        const uint64_t p_filesz = lpr_exec_rd64(ph + 32);
        if (out_path[0] != '\0' ||
            p_filesz == 0 ||
            p_filesz >= out_size ||
            p_filesz > LPR_EXEC_MAX_INTERP_BYTES ||
            p_offset > file->size ||
            p_filesz > file->size - p_offset)
        {
            return -8;
        }
        int status = lpr_exec_read_file_range(runtime, file, p_offset, (unsigned char *)out_path, p_filesz);
        if (status != 0) {
            out_path[0] = '\0';
            return status;
        }
        if (out_path[p_filesz - 1u] != '\0' || out_path[0] != '/') {
            out_path[0] = '\0';
            return -8;
        }
    }
    return 0;
}

int lpr_exec_image_find_text_section(const lpr_exec_image_t *image, uint64_t *out_offset, uint64_t *out_size)
{
    if (out_offset == NULL || out_size == NULL) {
        return -22;
    }
    *out_offset = 0;
    *out_size = 0;
    int status = lpr_exec_validate_elf(image);
    if (status != 0) {
        return status;
    }
    const unsigned char *bytes = image->bytes;
    const uint64_t e_shoff = lpr_exec_rd64(bytes + 40);
    const uint16_t e_shentsize = lpr_exec_rd16(bytes + 58);
    const uint16_t e_shnum = lpr_exec_rd16(bytes + 60);
    const uint16_t e_shstrndx = lpr_exec_rd16(bytes + 62);
    if (e_shoff == 0 || e_shnum == 0) {
        return 0;
    }
    if (e_shentsize < LPR_EXEC_SHDR_BYTES || e_shstrndx >= e_shnum ||
        e_shoff > image->size || (uint64_t)e_shentsize * e_shnum > image->size - e_shoff)
    {
        return -8;
    }

    const unsigned char *shstr = bytes + e_shoff + (uint64_t)e_shstrndx * e_shentsize;
    const uint64_t shstr_offset = lpr_exec_rd64(shstr + 24);
    const uint64_t shstr_size = lpr_exec_rd64(shstr + 32);
    if (shstr_offset > image->size || shstr_size > image->size - shstr_offset) {
        return -8;
    }
    const char *names = (const char *)(const void *)(bytes + shstr_offset);
    for (uint16_t i = 0; i < e_shnum; ++i) {
        const unsigned char *sh = bytes + e_shoff + (uint64_t)i * e_shentsize;
        const uint32_t sh_name = lpr_exec_rd32(sh);
        const uint64_t sh_offset = lpr_exec_rd64(sh + 24);
        const uint64_t sh_size = lpr_exec_rd64(sh + 32);
        if (sh_name >= shstr_size) {
            continue;
        }
        if (strcmp(names + sh_name, ".text") != 0) {
            continue;
        }
        if (sh_offset > image->size || sh_size > image->size - sh_offset) {
            return -8;
        }
        *out_offset = sh_offset;
        *out_size = sh_size;
        return 0;
    }
    return 0;
}

int lpr_exec_image_find_symbol(const lpr_exec_image_t *image, const char *symbol, uint64_t *out_value)
{
    if (image == NULL || image->bytes == NULL || symbol == NULL || out_value == NULL) {
        return -22;
    }
    *out_value = 0;
    if (image->size < LPR_EXEC_EHDR_BYTES) {
        return -8;
    }
    const unsigned char *bytes = image->bytes;
    const uint64_t e_shoff = lpr_exec_rd64(bytes + 40);
    const uint16_t e_shentsize = lpr_exec_rd16(bytes + 58);
    const uint16_t e_shnum = lpr_exec_rd16(bytes + 60);
    if (e_shoff == 0 || e_shentsize < LPR_EXEC_SHDR_BYTES || e_shnum == 0 ||
        e_shoff > image->size || (uint64_t)e_shentsize * e_shnum > image->size - e_shoff)
    {
        return -8;
    }
    for (uint16_t i = 0; i < e_shnum; ++i) {
        const unsigned char *sh = bytes + e_shoff + (uint64_t)i * e_shentsize;
        const uint32_t sh_type = lpr_exec_rd32(sh + 4);
        if (sh_type != LPR_EXEC_SHT_SYMTAB && sh_type != LPR_EXEC_SHT_DYNSYM) {
            continue;
        }
        const uint64_t sh_offset = lpr_exec_rd64(sh + 24);
        const uint64_t sh_size = lpr_exec_rd64(sh + 32);
        const uint32_t sh_link = lpr_exec_rd32(sh + 40);
        const uint64_t sh_entsize = lpr_exec_rd64(sh + 56);
        if (sh_entsize < LPR_EXEC_SYM_BYTES || sh_link >= e_shnum ||
            sh_offset > image->size || sh_size > image->size - sh_offset)
        {
            return -8;
        }
        const unsigned char *str_sh = bytes + e_shoff + (uint64_t)sh_link * e_shentsize;
        const uint64_t str_offset = lpr_exec_rd64(str_sh + 24);
        const uint64_t str_size = lpr_exec_rd64(str_sh + 32);
        if (str_offset > image->size || str_size > image->size - str_offset) {
            return -8;
        }
        const char *strings = (const char *)(const void *)(bytes + str_offset);
        const uint64_t count = sh_size / sh_entsize;
        for (uint64_t j = 0; j < count; ++j) {
            const unsigned char *sym = bytes + sh_offset + j * sh_entsize;
            const uint32_t st_name = lpr_exec_rd32(sym);
            if (st_name >= str_size) {
                continue;
            }
            const char *name = strings + st_name;
            if (strcmp(name, symbol) == 0) {
                *out_value = lpr_exec_rd64(sym + 8);
                return 0;
            }
        }
    }
    return -2;
}

int lpr_exec_patch_syscalls(
    unsigned char *bytes,
    uint64_t size,
    uint64_t *out_patched)
{
    if (bytes == NULL || out_patched == NULL) {
        return -22;
    }
    *out_patched = 0;
    lpr_patch_scan_result_t result;
    if (lpr_patch_scan_syscalls(
            bytes, size, LPR_PATCH_SCAN_START_BOUNDARY, &result) != 0)
    {
        return -8;
    }
    *out_patched = result.patched_sites;
    return 0;
}
