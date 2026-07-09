#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <personality/lpr_image_abi.h>

#include "filed/payload_v2.h"
#include "filed/runtime.h"
#include "filed/vfs.h"
#include "pacha/abi.h"
#include "pacha/ipc.h"

enum {
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
    LPR_EXEC_MAX_IMAGE_BYTES = 64ull * 1024ull * 1024ull,
    LPR_EXEC_MAX_INTERP_BYTES = 256,
    LPR_EXEC_WALK_RIGHTS =
        FILED_RIGHT_LOOKUP |
        FILED_RIGHT_STAT |
        FILED_RIGHT_GETDENTS,
};

_Static_assert(LPR_IMAGE_PAGE_SIZE == 4096ull, "filed exec page size must match the LPR image ABI");
_Static_assert(LPR_IMAGE_AT_BOOTSTRAP_FD == PACHA_AT_BOOTSTRAP_FD, "bootstrap auxv key must match the Pacha ABI");

typedef struct lpr_exec_image {
    unsigned char *bytes;
    uint64_t backend_object;
    uint64_t object_generation;
    uint64_t size;
} lpr_exec_image_t;

typedef struct lpr_exec_file {
    filed_handle_id_t handle_id;
    uint64_t backend_object;
    uint64_t object_generation;
    uint64_t size;
} lpr_exec_file_t;

typedef struct lpr_exec_meta {
    unsigned char ehdr[LPR_EXEC_EHDR_BYTES];
    unsigned char *phdrs;
    char interp_path[LPR_EXEC_MAX_INTERP_BYTES];
    uint64_t phdr_bytes;
    uint64_t text_offset;
    uint64_t text_size;
    uint64_t entry;
    uint64_t phoff;
    uint16_t type;
    uint16_t phent;
    uint16_t phnum;
} lpr_exec_meta_t;

typedef struct lpr_exec_loaded {
    uint64_t entry;
    uint64_t base;
    uint64_t phdr_va;
    uint16_t phent;
    uint16_t phnum;
    uint16_t load_segments;
} lpr_exec_loaded_t;

typedef struct lpr_exec_pending_map_batch {
    struct pacha_process_map_batch_entry entries[PACHA_PROCESS_MAP_BATCH_MAX_ENTRIES];
    int close_fds[PACHA_PROCESS_MAP_BATCH_MAX_ENTRIES];
    uint64_t count;
} lpr_exec_pending_map_batch_t;

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
    return value & ~(uint64_t)(LPR_IMAGE_PAGE_SIZE - 1);
}

static inline int lpr_exec_align_up(uint64_t value, uint64_t *out)
{
    if (out == NULL || value > UINT64_MAX - (LPR_IMAGE_PAGE_SIZE - 1)) {
        return -75;
    }
    *out = (value + (LPR_IMAGE_PAGE_SIZE - 1)) & ~(uint64_t)(LPR_IMAGE_PAGE_SIZE - 1);
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
int lpr_exec_read_full_file_image(
    filed_runtime_t *runtime,
    const lpr_exec_file_t *file,
    lpr_exec_image_t *out_image);
uint64_t lpr_exec_now_ns(void);
uint64_t lpr_exec_now_cycles(void);
void lpr_exec_metric(const char *label, uint64_t start_ns, uint64_t end_ns);
void lpr_exec_metric_cycles(const char *label, uint64_t start_cycles, uint64_t end_cycles);
int lpr_exec_read_absolute_image(filed_runtime_t *runtime, const char *path, lpr_exec_image_t *out_image);
int lpr_exec_init_file_from_handle(filed_runtime_t *runtime, filed_handle_id_t handle_id, lpr_exec_file_t *out_file);
int lpr_exec_open_absolute_file(filed_runtime_t *runtime, const char *path, lpr_exec_file_t *out_file);
void lpr_exec_close_file(filed_runtime_t *runtime, lpr_exec_file_t *file);
int lpr_exec_read_file_range(
    filed_runtime_t *runtime,
    const lpr_exec_file_t *file,
    uint64_t offset,
    unsigned char *buffer,
    uint64_t length);
int lpr_exec_read_file_range_for_load(
    filed_runtime_t *runtime,
    const lpr_exec_file_t *file,
    uint64_t offset,
    uint64_t length,
    unsigned char **out_buffer,
    bool *out_owned);
int lpr_exec_read_meta(filed_runtime_t *runtime, const lpr_exec_file_t *file, lpr_exec_meta_t *out_meta);
void lpr_exec_free_meta(lpr_exec_meta_t *meta);

int lpr_exec_validate_elf(const lpr_exec_image_t *image);
int lpr_exec_get_interp_path(const lpr_exec_image_t *image, char *out_path, size_t out_size);
int lpr_exec_meta_get_interp_path(
    filed_runtime_t *runtime,
    const lpr_exec_file_t *file,
    const lpr_exec_meta_t *meta,
    char *out_path,
    size_t out_size);
int lpr_exec_image_find_text_section(const lpr_exec_image_t *image, uint64_t *out_offset, uint64_t *out_size);
int lpr_exec_image_find_symbol(const lpr_exec_image_t *image, const char *symbol, uint64_t *out_value);
uint64_t lpr_exec_patch_syscalls(unsigned char *bytes, uint64_t size);
void lpr_exec_image_dump_metrics(void);
void lpr_exec_map_dump_metrics(void);

void lpr_exec_invalidate_segment_vmo_cache(uint64_t backend_object);
void lpr_exec_invalidate_runtime_image_cache(uint64_t backend_object);
void lpr_exec_invalidate_interpreter_cache(filed_runtime_t *runtime, uint64_t backend_object);
int lpr_exec_install_low_layout(int process_fd, uint64_t syscall_entry_va);
int lpr_exec_load_image_into_process(
    int process_fd,
    const lpr_exec_image_t *image,
    uint64_t dyn_base,
    int patch_text,
    lpr_exec_loaded_t *loaded);
int lpr_exec_load_image_with_low_layout_into_process(
    int process_fd,
    const lpr_exec_image_t *image,
    uint64_t dyn_base,
    int patch_text,
    uint64_t syscall_entry_offset,
    lpr_exec_loaded_t *loaded);
int lpr_exec_load_file_into_process(
    filed_runtime_t *runtime,
    int process_fd,
    const lpr_exec_file_t *file,
    const lpr_exec_meta_t *meta,
    uint64_t dyn_base,
    int patch_text,
    lpr_exec_loaded_t *loaded);
void lpr_exec_pending_map_batch_init(lpr_exec_pending_map_batch_t *batch);
void lpr_exec_pending_map_batch_discard(lpr_exec_pending_map_batch_t *batch);
int lpr_exec_pending_map_batch_commit(int process_fd, lpr_exec_pending_map_batch_t *batch);
int lpr_exec_prepare_file_into_map_batch(
    filed_runtime_t *runtime,
    const lpr_exec_file_t *file,
    const lpr_exec_meta_t *meta,
    uint64_t dyn_base,
    int patch_text,
    lpr_exec_pending_map_batch_t *batch,
    lpr_exec_loaded_t *loaded);

int lpr_exec_prepare_inherit_fds(
    const filed_v2_exec_path_t *request,
    const int *inherit_fds,
    uint64_t inherit_fd_count,
    int bootstrap_fd,
    int *prepared,
    uint64_t *out_prepared_count);
void lpr_exec_clear_prepared_inherit_fds(const int *prepared, uint64_t count);
int lpr_exec_start_plan(lpr_exec_plan_t *plan, const filed_v2_exec_path_t *request, int bootstrap_fd, int start_thread);
void lpr_exec_discard_process_fd(int process_fd);
