#include "internal.h"
#include "filed/storage.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Linux owns all cached file state. Each exec reads through its real open file;
 * keeping pathname/image snapshots here would reintroduce stale exec images. */
int lpr_exec_read_file_range(filed_runtime_t *rt, const lpr_exec_file_t *file,
                             uint64_t offset, unsigned char *bytes,
                             uint64_t length)
{
    if (!rt || !file || !file->handle_id || (length && !bytes) ||
        offset > file->size || length > file->size - offset ||
        length > SIZE_MAX) return -22;
    size_t done = 0;
    while (done < length) {
        size_t got = 0;
        int status = filed_storage_exec_pread(rt, file->handle_id, offset + done,
                                         bytes + done, (size_t)length - done, &got);
        if (status) return status;
        if (!got) return -8;
        done += got;
    }
    return 0;
}
int lpr_exec_read_file_range_for_load(filed_runtime_t *rt,
    const lpr_exec_file_t *file, uint64_t offset, uint64_t length,
    unsigned char **out, bool *owned)
{
    if (!out || !owned || length > SIZE_MAX) return -22;
    *out = NULL;
    *owned = true;
    if (!length) return 0;
    unsigned char *bytes = malloc((size_t)length);
    if (!bytes) return -12;
    int status = lpr_exec_read_file_range(rt, file, offset, bytes, length);
    if (status) { free(bytes); return status; }
    *out = bytes;
    return 0;
}
int lpr_exec_init_file_from_handle(filed_runtime_t *rt, uint64_t handle,
                                  lpr_exec_file_t *out)
{
    if (!rt || !out || !handle) return -22;
    filed_storage_stat_t stat;
    int status = filed_storage_exec_stat(rt, handle, &stat);
    if (status) return status;
    if ((stat.mode & 0170000) != 0100000) return -8;
    *out = (lpr_exec_file_t){ .handle_id = handle, .size = stat.size };
    return 0;
}
int lpr_exec_read_full_file_image(filed_runtime_t *rt,
    const lpr_exec_file_t *file, lpr_exec_image_t *out)
{
    if (!file || !out || file->size < LPR_EXEC_EHDR_BYTES ||
        file->size > SIZE_MAX) return -8;
    memset(out, 0, sizeof(*out));
    unsigned char *bytes = malloc((size_t)file->size);
    if (!bytes) return -12;
    int status = lpr_exec_read_file_range(rt, file, 0, bytes, file->size);
    if (status) { free(bytes); return status; }
    *out = (lpr_exec_image_t){ .bytes = bytes, .size = file->size };
    return 0;
}
int lpr_exec_read_full_image(filed_runtime_t *rt, uint64_t handle,
                             lpr_exec_image_t *out)
{
    lpr_exec_file_t file;
    int status = lpr_exec_init_file_from_handle(rt, handle, &file);
    return status ? status : lpr_exec_read_full_file_image(rt, &file, out);
}
static int lpr_exec_meta_read_text_section(
    filed_runtime_t *runtime,
    const lpr_exec_file_t *file,
    const unsigned char *prefix,
    uint64_t prefix_size,
    lpr_exec_meta_t *meta)
{
    if (runtime == NULL || file == NULL || prefix == NULL || meta == NULL) {
        return -22;
    }
    meta->text_offset = 0;
    meta->text_size = 0;

    const unsigned char *ehdr = meta->ehdr;
    const uint64_t e_shoff = lpr_exec_rd64(ehdr + 40);
    const uint16_t e_shentsize = lpr_exec_rd16(ehdr + 58);
    const uint16_t e_shnum = lpr_exec_rd16(ehdr + 60);
    const uint16_t e_shstrndx = lpr_exec_rd16(ehdr + 62);
    if (e_shoff == 0 || e_shnum == 0) {
        return 0;
    }
    if (e_shentsize < LPR_EXEC_SHDR_BYTES || e_shstrndx >= e_shnum || e_shoff > file->size) {
        return -8;
    }
    const uint64_t shdr_bytes = (uint64_t)e_shentsize * (uint64_t)e_shnum;
    if (shdr_bytes > file->size - e_shoff || shdr_bytes > SIZE_MAX) {
        return -8;
    }

    unsigned char *shdrs = malloc((size_t)shdr_bytes);
    if (shdrs == NULL) {
        return -12;
    }
    int status = 0;
    if (e_shoff <= prefix_size && shdr_bytes <= prefix_size - e_shoff) {
        memcpy(shdrs, prefix + e_shoff, (size_t)shdr_bytes);
    } else {
        status = lpr_exec_read_file_range(runtime, file, e_shoff, shdrs, shdr_bytes);
        if (status != 0) {
            free(shdrs);
            return status;
        }
    }

    const unsigned char *shstr = shdrs + (uint64_t)e_shstrndx * e_shentsize;
    const uint64_t shstr_offset = lpr_exec_rd64(shstr + 24);
    const uint64_t shstr_size = lpr_exec_rd64(shstr + 32);
    if (shstr_offset > file->size || shstr_size > file->size - shstr_offset || shstr_size > SIZE_MAX) {
        free(shdrs);
        return -8;
    }

    unsigned char *names = malloc((size_t)shstr_size);
    if (names == NULL) {
        free(shdrs);
        return -12;
    }
    if (shstr_offset <= prefix_size && shstr_size <= prefix_size - shstr_offset) {
        memcpy(names, prefix + shstr_offset, (size_t)shstr_size);
    } else {
        status = lpr_exec_read_file_range(runtime, file, shstr_offset, names, shstr_size);
        if (status != 0) {
            free(names);
            free(shdrs);
            return status;
        }
    }

    for (uint16_t i = 0; i < e_shnum; ++i) {
        const unsigned char *sh = shdrs + (uint64_t)i * e_shentsize;
        const uint32_t sh_name = lpr_exec_rd32(sh);
        const uint64_t sh_offset = lpr_exec_rd64(sh + 24);
        const uint64_t sh_size = lpr_exec_rd64(sh + 32);
        if (sh_name >= shstr_size || strcmp((const char *)(const void *)(names + sh_name), ".text") != 0) {
            continue;
        }
        if (sh_offset > file->size || sh_size > file->size - sh_offset) {
            free(names);
            free(shdrs);
            return -8;
        }
        meta->text_offset = sh_offset;
        meta->text_size = sh_size;
        break;
    }

    free(names);
    free(shdrs);
    return 0;
}


int lpr_exec_read_meta(filed_runtime_t *runtime, const lpr_exec_file_t *file, lpr_exec_meta_t *out_meta)
{
    unsigned char prefix[LPR_IMAGE_PAGE_SIZE];
    uint64_t prefix_size = 0;

    if (runtime == NULL || file == NULL || out_meta == NULL) {
        return -22;
    }
    memset(out_meta, 0, sizeof(*out_meta));
    if (file->size < LPR_EXEC_EHDR_BYTES) {
        return -8;
    }
    memset(prefix, 0, sizeof(prefix));
    prefix_size = file->size < sizeof(prefix) ? file->size : sizeof(prefix);
    int status = lpr_exec_read_file_range(runtime, file, 0, prefix, prefix_size);
    if (status != 0) {
        return status;
    }
    memcpy(out_meta->ehdr, prefix, LPR_EXEC_EHDR_BYTES);
    const unsigned char *ehdr = out_meta->ehdr;
    if (ehdr[0] != 0x7f || ehdr[1] != 'E' || ehdr[2] != 'L' || ehdr[3] != 'F') {
        return -8;
    }
    if (ehdr[4] != LPR_EXEC_ELF_CLASS_64 ||
        ehdr[5] != LPR_EXEC_ELF_DATA_LSB ||
        ehdr[6] != LPR_EXEC_ELF_VERSION_CURRENT)
    {
        return -8;
    }
    out_meta->type = lpr_exec_rd16(ehdr + 16);
    const uint16_t e_machine = lpr_exec_rd16(ehdr + 18);
    const uint32_t e_version = lpr_exec_rd32(ehdr + 20);
    out_meta->entry = lpr_exec_rd64(ehdr + 24);
    out_meta->phoff = lpr_exec_rd64(ehdr + 32);
    out_meta->phent = lpr_exec_rd16(ehdr + 54);
    out_meta->phnum = lpr_exec_rd16(ehdr + 56);
    if ((out_meta->type != LPR_EXEC_ELF_TYPE_EXEC && out_meta->type != LPR_EXEC_ELF_TYPE_DYN) ||
        e_machine != LPR_EXEC_ELF_MACHINE_X86_64 ||
        e_version != LPR_EXEC_ELF_VERSION_CURRENT ||
        out_meta->phent < LPR_EXEC_PHDR_BYTES ||
        out_meta->phnum == 0)
    {
        return -8;
    }
    if (out_meta->phoff > file->size) {
        return -8;
    }
    out_meta->phdr_bytes = (uint64_t)out_meta->phent * (uint64_t)out_meta->phnum;
    if (out_meta->phdr_bytes > file->size - out_meta->phoff ||
        out_meta->phdr_bytes > SIZE_MAX)
    {
        return -8;
    }
    out_meta->phdrs = malloc((size_t)out_meta->phdr_bytes);
    if (out_meta->phdrs == NULL) {
        return -12;
    }
    if (out_meta->phoff <= prefix_size &&
        out_meta->phdr_bytes <= prefix_size - out_meta->phoff)
    {
        memcpy(out_meta->phdrs, prefix + out_meta->phoff, (size_t)out_meta->phdr_bytes);
    } else {
        status = lpr_exec_read_file_range(runtime, file, out_meta->phoff, out_meta->phdrs, out_meta->phdr_bytes);
        if (status != 0) {
            lpr_exec_free_meta(out_meta);
            return status;
        }
    }
    status = lpr_exec_meta_read_text_section(runtime, file, prefix, prefix_size, out_meta);
    if (status != 0) {
        lpr_exec_free_meta(out_meta);
        return status;
    }
    for (uint16_t i = 0; i < out_meta->phnum; ++i) {
        const unsigned char *ph = out_meta->phdrs + (uint64_t)i * out_meta->phent;
        if (lpr_exec_rd32(ph) != LPR_EXEC_PT_INTERP) {
            continue;
        }
        const uint64_t p_offset = lpr_exec_rd64(ph + 8);
        const uint64_t p_filesz = lpr_exec_rd64(ph + 32);
        if (out_meta->interp_path[0] != '\0' ||
            p_filesz == 0 ||
            p_filesz >= sizeof(out_meta->interp_path) ||
            p_offset > file->size ||
            p_filesz > file->size - p_offset)
        {
            lpr_exec_free_meta(out_meta);
            return -8;
        }
        if (p_offset <= prefix_size && p_filesz <= prefix_size - p_offset) {
            if (prefix[p_offset + p_filesz - 1u] != '\0' || prefix[p_offset] != '/') {
                lpr_exec_free_meta(out_meta);
                return -8;
            }
            memcpy(out_meta->interp_path, prefix + p_offset, (size_t)p_filesz);
        } else {
            status = lpr_exec_read_file_range(runtime, file, p_offset, (unsigned char *)out_meta->interp_path, p_filesz);
            if (status != 0) {
                lpr_exec_free_meta(out_meta);
                return status;
            }
            if (out_meta->interp_path[p_filesz - 1u] != '\0' || out_meta->interp_path[0] != '/') {
                lpr_exec_free_meta(out_meta);
                return -8;
            }
        }
    }
    return 0;
}

void lpr_exec_free_meta(lpr_exec_meta_t *meta)
{
    if (meta == NULL) {
        return;
    }
    free(meta->phdrs);
    memset(meta, 0, sizeof(*meta));
}


int lpr_exec_open_file_at(filed_runtime_t *rt, uint64_t directory,
                         const char *path, lpr_exec_file_t *out)
{
    if (!rt || !path || !out) return -22;
    uint64_t handle = 0;
    int status=filed_storage_exec_open(rt,directory,path,&handle);
    if (!status) status = lpr_exec_init_file_from_handle(rt, handle, out);
    if (status && handle) (void)filed_storage_exec_close(rt, handle);
    return status;
}
int lpr_exec_open_absolute_file(filed_runtime_t *rt, const char *path,
                                lpr_exec_file_t *out)
{
    return lpr_exec_open_file_at(rt, 0, path, out);
}
void lpr_exec_close_file(filed_runtime_t *rt, lpr_exec_file_t *file)
{
    if (rt && file && file->handle_id) {
        (void)filed_storage_exec_close(rt, file->handle_id);
        memset(file, 0, sizeof(*file));
    }
}
int lpr_exec_read_absolute_image(filed_runtime_t *rt, const char *path,
                                lpr_exec_image_t *out)
{
    lpr_exec_file_t file;
    int status = lpr_exec_open_absolute_file(rt, path, &file);
    if (status) return status;
    status = lpr_exec_read_full_file_image(rt, &file, out);
    lpr_exec_close_file(rt, &file);
    return status;
}
