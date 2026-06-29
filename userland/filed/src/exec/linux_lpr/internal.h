#pragma once

#include <stdint.h>
#include <stddef.h>

#include <personality/zpoline.h>

#include "filed/ipc_protocol.h"
#include "filed/runtime.h"
#include "filed/vfs.h"
#include "pacha/abi.h"

enum {
    LPR_EXEC_PAGE_SIZE = 4096,
    LPR_EXEC_EHDR_BYTES = 64,
    LPR_EXEC_PHDR_BYTES = 56,
    LPR_EXEC_SHDR_BYTES = 64,
    LPR_EXEC_SYM_BYTES = 24,
    LPR_EXEC_ELF_CLASS_64 = 2,
    LPR_EXEC_ELF_DATA_LSB = 1,
    LPR_EXEC_ELF_VERSION_CURRENT = 1,
    LPR_EXEC_ELF_TYPE_EXEC = 2,
    LPR_EXEC_ELF_TYPE_DYN = 3,
    LPR_EXEC_ELF_MACHINE_X86_64 = 0x3e,
    LPR_EXEC_PT_LOAD = 1,
    LPR_EXEC_PT_INTERP = 3,
    LPR_EXEC_PT_PHDR = 6,
    LPR_EXEC_PF_X = 1,
    LPR_EXEC_PF_W = 2,
    LPR_EXEC_PF_R = 4,
    LPR_EXEC_SHT_SYMTAB = 2,
    LPR_EXEC_SHT_DYNSYM = 11,
    LPR_EXEC_AT_NULL = 0,
    LPR_EXEC_AT_PHDR = 3,
    LPR_EXEC_AT_PHENT = 4,
    LPR_EXEC_AT_PHNUM = 5,
    LPR_EXEC_AT_PAGESZ = 6,
    LPR_EXEC_AT_BASE = 7,
    LPR_EXEC_AT_ENTRY = 9,
    LPR_EXEC_AT_RANDOM = 25,
    LPR_EXEC_AT_EXECFN = 31,
    LPR_EXEC_MAX_IMAGE_BYTES = 64ull * 1024ull * 1024ull,
    LPR_EXEC_MAX_INTERP_BYTES = 256,
    LPR_EXEC_LPR_BASE = 0x04000000ull,
    LPR_EXEC_MAIN_DYN_BASE = 0x10000000ull,
    LPR_EXEC_INTERP_DYN_BASE = 0x20000000ull,
    LPR_EXEC_WALK_RIGHTS =
        FILED_RIGHT_LOOKUP |
        FILED_RIGHT_STAT |
        FILED_RIGHT_GETDENTS,
};

#define LPR_EXEC_RUNTIME_PATH "/lib/pacha/lpr-linux-x86_64.so"

typedef struct lpr_exec_image {
    unsigned char *bytes;
    uint64_t size;
} lpr_exec_image_t;

typedef struct lpr_exec_loaded {
    uint64_t entry;
    uint64_t base;
    uint64_t phdr_va;
    uint16_t phent;
    uint16_t phnum;
    uint16_t load_segments;
} lpr_exec_loaded_t;

typedef struct lpr_exec_plan {
    int process_fd;
    int thread_fd;
    uint64_t main_entry;
    uint64_t runtime_entry;
    uint64_t interpreter_base;
    uint64_t phdr_va;
    uint64_t phent;
    uint64_t phnum;
} lpr_exec_plan_t;

static inline uint16_t lpr_exec_rd16(const unsigned char *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t lpr_exec_rd32(const unsigned char *p)
{
    return (uint32_t)p[0] |
        ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) |
        ((uint32_t)p[3] << 24);
}

static inline uint64_t lpr_exec_rd64(const unsigned char *p)
{
    return (uint64_t)lpr_exec_rd32(p) | ((uint64_t)lpr_exec_rd32(p + 4) << 32);
}

static inline void lpr_exec_wr64(unsigned char *p, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) {
        p[i] = (unsigned char)((value >> (i * 8u)) & 0xffu);
    }
}

static inline uint64_t lpr_exec_align_down(uint64_t value)
{
    return value & ~(uint64_t)(LPR_EXEC_PAGE_SIZE - 1);
}

static inline int lpr_exec_align_up(uint64_t value, uint64_t *out)
{
    if (out == NULL || value > UINT64_MAX - (LPR_EXEC_PAGE_SIZE - 1)) {
        return -75;
    }
    *out = (value + (LPR_EXEC_PAGE_SIZE - 1)) & ~(uint64_t)(LPR_EXEC_PAGE_SIZE - 1);
    return 0;
}

static inline uint64_t lpr_exec_prot_from_elf_flags(uint32_t flags)
{
    uint64_t prot = 0;
    if ((flags & LPR_EXEC_PF_R) != 0) prot |= PACHA_PROT_READ;
    if ((flags & LPR_EXEC_PF_W) != 0) prot |= PACHA_PROT_WRITE;
    if ((flags & LPR_EXEC_PF_X) != 0) prot |= PACHA_PROT_EXEC;
    return prot;
}

static inline int lpr_exec_status_to_errno(filed_status_t status)
{
    switch (status) {
    case FILED_OK:
        return 0;
    case FILED_ERR_NOT_FOUND:
        return -2;
    case FILED_ERR_NOT_DIR:
        return -20;
    case FILED_ERR_IS_DIR:
        return -21;
    case FILED_ERR_DENIED:
        return -13;
    case FILED_ERR_FULL:
    case FILED_ERR_OVERFLOW:
        return -75;
    case FILED_ERR_IO:
        return -5;
    case FILED_ERR_BAD_FORMAT:
    case FILED_ERR_INVALID_IMAGE:
        return -8;
    case FILED_ERR_UNSUPPORTED:
        return -95;
    case FILED_ERR_INVALID:
    default:
        return -22;
    }
}

int lpr_exec_read_full_image(filed_runtime_t *runtime, filed_handle_id_t handle_id, lpr_exec_image_t *out_image);
int lpr_exec_read_absolute_image(filed_runtime_t *runtime, const char *path, lpr_exec_image_t *out_image);

int lpr_exec_validate_elf(const lpr_exec_image_t *image);
int lpr_exec_get_interp_path(const lpr_exec_image_t *image, char *out_path, size_t out_size);
int lpr_exec_image_find_symbol(const lpr_exec_image_t *image, const char *symbol, uint64_t *out_value);
void lpr_exec_patch_syscalls(unsigned char *bytes, uint64_t size);

int lpr_exec_install_low_layout(int process_fd, uint64_t syscall_entry_va);
int lpr_exec_load_image_into_process(
    int process_fd,
    const lpr_exec_image_t *image,
    uint64_t dyn_base,
    int patch_text,
    lpr_exec_loaded_t *loaded);

int lpr_exec_prepare_inherit_fds(
    const filed_wire_exec_path_t *request,
    const int *inherit_fds,
    uint64_t inherit_fd_count,
    int bootstrap_fd,
    int *prepared,
    uint64_t *out_prepared_count);
void lpr_exec_clear_prepared_inherit_fds(const int *prepared, uint64_t count);
int lpr_exec_start_plan(lpr_exec_plan_t *plan, const filed_wire_exec_path_t *request, int bootstrap_fd);
void lpr_exec_discard_process_fd(int process_fd);
