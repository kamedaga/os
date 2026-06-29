#include "internal.h"

#include <stdlib.h>
#include <string.h>

#include "pacha/ipc.h"

static int fill_vmo(int vmo_fd, const void *initial_bytes, uint64_t initial_size, uint64_t map_size)
{
    if (initial_size == 0) {
        return 0;
    }
    if (initial_bytes == NULL || initial_size > map_size) {
        return -22;
    }
    unsigned char *mapped = pacha_mmap(vmo_fd, map_size, PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (mapped == NULL) {
        return -12;
    }
    memset(mapped, 0, (size_t)map_size);
    memcpy(mapped, initial_bytes, (size_t)initial_size);
    (void)pacha_munmap(mapped, map_size);
    return 0;
}

static int map_vmo_fixed(
    int process_fd,
    uint64_t target_va,
    uint64_t map_size,
    uint64_t prot,
    const void *initial_bytes,
    uint64_t initial_size,
    uint64_t *out_mapped)
{
    const uint64_t rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE |
        PACHA_FD_RIGHT_MAP_EXEC;
    const int vmo_fd = pacha_vmo_create(map_size, rights, 0);
    if (vmo_fd < 16) {
        return -12;
    }
    int status = fill_vmo(vmo_fd, initial_bytes, initial_size, map_size);
    if (status != 0) {
        (void)pacha_fd_close(vmo_fd);
        return status;
    }
    const long map_result = pacha_process_map(process_fd, vmo_fd, target_va, map_size, prot, 0);
    (void)pacha_fd_close(vmo_fd);
    if (target_va == PACHA_PROCESS_MAP_ANYWHERE) {
        if (map_result < 4096) {
            return -12;
        }
    } else if ((uint64_t)map_result != target_va) {
        return -12;
    }
    if (out_mapped != NULL) {
        *out_mapped = (uint64_t)map_result;
    }
    return 0;
}

static int build_zpoline_page(unsigned char *page, uint64_t handler_va)
{
    if (page == NULL || handler_va == 0) {
        return -22;
    }
    memset(page, 0x90, LPR_ZPOLINE_PAGE_SIZE);
    page[LPR_ZPOLINE_SHIM_OFFSET + 0] = 0x49;
    page[LPR_ZPOLINE_SHIM_OFFSET + 1] = 0xbb;
    lpr_exec_wr64(page + LPR_ZPOLINE_SHIM_OFFSET + 2, handler_va);
    page[LPR_ZPOLINE_SHIM_OFFSET + 10] = 0x41;
    page[LPR_ZPOLINE_SHIM_OFFSET + 11] = 0xff;
    page[LPR_ZPOLINE_SHIM_OFFSET + 12] = 0xe3;
    return 0;
}

int lpr_exec_install_low_layout(int process_fd, uint64_t syscall_entry_va)
{
    unsigned char zpoline[LPR_ZPOLINE_PAGE_SIZE];
    int status = build_zpoline_page(zpoline, syscall_entry_va);
    if (status != 0) {
        return status;
    }
    status = map_vmo_fixed(
        process_fd,
        LPR_ZPOLINE_PAGE_VA,
        LPR_ZPOLINE_PAGE_SIZE,
        PACHA_PROT_READ | PACHA_PROT_EXEC,
        zpoline,
        sizeof(zpoline),
        NULL);
    if (status != 0) {
        return status;
    }
    return 0;
}

static int range_overlaps(uint64_t a, uint64_t asz, uint64_t b, uint64_t bsz)
{
    if (asz == 0 || bsz == 0 || a > UINT64_MAX - asz || b > UINT64_MAX - bsz) {
        return 1;
    }
    return a < b + bsz && b < a + asz;
}

static int map_segment(
    int process_fd,
    const lpr_exec_image_t *image,
    const unsigned char *ph,
    uint64_t load_bias,
    int patch_text,
    uint64_t *out_mapped)
{
    const uint32_t p_flags = lpr_exec_rd32(ph + 4);
    const uint64_t p_offset = lpr_exec_rd64(ph + 8);
    const uint64_t p_vaddr = lpr_exec_rd64(ph + 16);
    const uint64_t p_filesz = lpr_exec_rd64(ph + 32);
    const uint64_t p_memsz = lpr_exec_rd64(ph + 40);
    if (p_filesz > p_memsz ||
        p_offset > image->size ||
        p_filesz > image->size - p_offset ||
        p_vaddr > UINT64_MAX - load_bias)
    {
        return -8;
    }
    if (p_memsz == 0) {
        if (out_mapped != NULL) *out_mapped = 0;
        return 0;
    }
    const uint64_t target_va = lpr_exec_align_down(p_vaddr + load_bias);
    const uint64_t page_offset = (p_vaddr + load_bias) - target_va;
    uint64_t map_size = 0;
    int status = lpr_exec_align_up(page_offset + p_memsz, &map_size);
    if (status != 0) {
        return status;
    }
    if (range_overlaps(target_va, map_size, LPR_ZPOLINE_PAGE_VA, LPR_LOW_GUARD_END_VA)) {
        return -8;
    }
    unsigned char *segment = malloc((size_t)map_size);
    if (segment == NULL) {
        return -12;
    }
    memset(segment, 0, (size_t)map_size);
    memcpy(segment + page_offset, image->bytes + p_offset, (size_t)p_filesz);
    if (patch_text && (p_flags & LPR_EXEC_PF_X) != 0) {
        lpr_exec_patch_syscalls(segment, map_size);
    }
    status = map_vmo_fixed(
        process_fd,
        target_va,
        map_size,
        lpr_exec_prot_from_elf_flags(p_flags),
        segment,
        map_size,
        out_mapped);
    free(segment);
    return status;
}

int lpr_exec_load_image_into_process(
    int process_fd,
    const lpr_exec_image_t *image,
    uint64_t dyn_base,
    int patch_text,
    lpr_exec_loaded_t *loaded)
{
    if (process_fd < 16 || image == NULL || loaded == NULL) {
        return -22;
    }
    memset(loaded, 0, sizeof(*loaded));
    int status = lpr_exec_validate_elf(image);
    if (status != 0) {
        return status;
    }
    const unsigned char *bytes = image->bytes;
    const uint64_t e_entry = lpr_exec_rd64(bytes + 24);
    const uint16_t e_type = lpr_exec_rd16(bytes + 16);
    const uint64_t e_phoff = lpr_exec_rd64(bytes + 32);
    const uint16_t e_phentsize = lpr_exec_rd16(bytes + 54);
    const uint16_t e_phnum = lpr_exec_rd16(bytes + 56);
    uint64_t load_bias = e_type == LPR_EXEC_ELF_TYPE_DYN ? dyn_base : 0;
    uint64_t phdr_va = 0;

    for (uint16_t i = 0; i < e_phnum; ++i) {
        const unsigned char *ph = bytes + e_phoff + (uint64_t)i * e_phentsize;
        const uint32_t p_type = lpr_exec_rd32(ph);
        if (p_type == LPR_EXEC_PT_PHDR) {
            phdr_va = lpr_exec_rd64(ph + 16);
            continue;
        }
        if (p_type != LPR_EXEC_PT_LOAD) {
            continue;
        }
        uint64_t mapped_va = 0;
        status = map_segment(process_fd, image, ph, load_bias, patch_text, &mapped_va);
        if (status != 0) {
            return status;
        }
        if (e_type == LPR_EXEC_ELF_TYPE_DYN && loaded->load_segments == 0) {
            load_bias = mapped_va - lpr_exec_align_down(lpr_exec_rd64(ph + 16));
        }
        loaded->load_segments++;
    }
    if (loaded->load_segments == 0) {
        return -8;
    }
    if (phdr_va == 0) {
        phdr_va = e_phoff;
    }
    loaded->entry = e_entry + load_bias;
    loaded->base = load_bias;
    loaded->phdr_va = phdr_va + load_bias;
    loaded->phent = e_phentsize;
    loaded->phnum = e_phnum;
    return 0;
}
