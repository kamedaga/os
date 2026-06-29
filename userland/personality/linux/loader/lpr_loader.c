#include "lpr_loader.h"

int lpr_loader_install_low_layout(
    void *context,
    const struct lpr_loader_low_ops *ops,
    uint64_t child_lpr_syscall_entry_va)
{
    if (ops == 0 || ops->map_fixed == 0 || child_lpr_syscall_entry_va == 0) {
        return LPR_LOADER_INVALID;
    }

    uint8_t zpoline_page[LPR_ZPOLINE_PAGE_SIZE];
    const int64_t build_status =
        lpr_build_zpoline_page(zpoline_page, child_lpr_syscall_entry_va);
    if (build_status != PERSONALITY_STATUS_OK) {
        return LPR_LOADER_INVALID;
    }

    struct lpr_low_mapping plan[LPR_LOW_LAYOUT_MAPPING_COUNT];
    lpr_low_layout_plan(plan);

    const struct lpr_loader_mapping_image zpoline = {
        .mapping = plan[0],
        .initial_bytes = zpoline_page,
        .initial_size = LPR_ZPOLINE_PAGE_SIZE,
    };
    if (ops->map_fixed(context, &zpoline) != 0) {
        return LPR_LOADER_MAP_FAILED;
    }

    const struct lpr_loader_mapping_image guard = {
        .mapping = plan[1],
        .initial_bytes = 0,
        .initial_size = 0,
    };
    if (ops->map_fixed(context, &guard) != 0) {
        return LPR_LOADER_MAP_FAILED;
    }

    return LPR_LOADER_OK;
}

int lpr_loader_patch_executable_mapping(
    uint8_t *bytes,
    uint64_t size_bytes,
    int executable,
    struct lpr_patch_mapping_result *out_result)
{
    if (out_result == 0) {
        return LPR_LOADER_INVALID;
    }
    out_result->scanned_bytes = 0;
    out_result->patched_sites = 0;
    out_result->skipped_sites = 0;
    out_result->failed_sites = 0;

    if (!executable || size_bytes == 0) {
        return LPR_LOADER_OK;
    }
    if (bytes == 0) {
        return LPR_LOADER_INVALID;
    }

    const struct lpr_patch_mapping_request request = {
        .start_va = (uint64_t)(uintptr_t)bytes,
        .size_bytes = size_bytes,
        .flags = LPR_PATCH_FLAG_EXECUTABLE | LPR_PATCH_FLAG_PRIVATE,
    };
    const int64_t status = lpr_patch_mapping(&request, out_result);
    return status == PERSONALITY_STATUS_OK ? LPR_LOADER_OK : LPR_LOADER_PATCH_FAILED;
}
