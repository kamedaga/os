#ifndef LPR_LOADER_H
#define LPR_LOADER_H

#include <stdint.h>
#include <personality/linux_lpr.h>
#include <personality/zpoline.h>
#include "lpr_low_layout.h"

enum lpr_loader_status {
    LPR_LOADER_OK = 0,
    LPR_LOADER_INVALID = 1,
    LPR_LOADER_MAP_FAILED = 2,
    LPR_LOADER_PATCH_FAILED = 3,
};

struct lpr_loader_mapping_image {
    struct lpr_low_mapping mapping;
    const void *initial_bytes;
    uint64_t initial_size;
};

typedef int (*lpr_loader_map_fixed_fn)(
    void *context,
    const struct lpr_loader_mapping_image *image);

struct lpr_loader_low_ops {
    lpr_loader_map_fixed_fn map_fixed;
};

int lpr_loader_install_low_layout(
    void *context,
    const struct lpr_loader_low_ops *ops,
    uint64_t child_lpr_syscall_entry_va);

int lpr_loader_patch_executable_mapping(
    uint8_t *bytes,
    uint64_t size_bytes,
    int executable,
    struct lpr_patch_mapping_result *out_result);

#endif
