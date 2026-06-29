#include "lpr_low_layout.h"

static int lpr_u64_range_overflows(uint64_t start, uint64_t size) {
    return size != 0 && start > UINT64_MAX - size;
}

static int lpr_ranges_overlap(uint64_t a_start, uint64_t a_size, uint64_t b_start, uint64_t b_size) {
    if (a_size == 0 || b_size == 0) return 0;
    if (lpr_u64_range_overflows(a_start, a_size) || lpr_u64_range_overflows(b_start, b_size)) return 1;
    const uint64_t a_end = a_start + a_size;
    const uint64_t b_end = b_start + b_size;
    return a_start < b_end && b_start < a_end;
}

void lpr_low_layout_plan(struct lpr_low_mapping out[LPR_LOW_LAYOUT_MAPPING_COUNT]) {
    if (out == 0) return;
    out[0] = (struct lpr_low_mapping) {
        .start_va = LPR_ZPOLINE_PAGE_VA,
        .size_bytes = LPR_ZPOLINE_PAGE_SIZE,
        .prot = LPR_LOW_PROT_READ | LPR_LOW_PROT_EXEC,
        .kind = LPR_LOW_MAPPING_ZPOLINE,
    };
    out[1] = (struct lpr_low_mapping) {
        .start_va = LPR_LOW_GUARD_START_VA,
        .size_bytes = LPR_LOW_GUARD_SIZE,
        .prot = LPR_LOW_PROT_NONE,
        .kind = LPR_LOW_MAPPING_GUARD,
    };
}

int lpr_low_layout_install(void *context, lpr_low_layout_map_fn map_fixed) {
    if (map_fixed == 0) return LPR_LOW_LAYOUT_INVALID;

    struct lpr_low_mapping plan[LPR_LOW_LAYOUT_MAPPING_COUNT];
    lpr_low_layout_plan(plan);
    for (uint32_t i = 0; i < LPR_LOW_LAYOUT_MAPPING_COUNT; i++) {
        const int status = map_fixed(context, &plan[i]);
        if (status != 0) return status;
    }
    return LPR_LOW_LAYOUT_OK;
}

int lpr_low_region_is_reserved(uint64_t start_va, uint64_t size_bytes) {
    if (size_bytes == 0) return 0;
    if (lpr_u64_range_overflows(start_va, size_bytes)) return LPR_LOW_LAYOUT_OVERFLOW;
    if (lpr_ranges_overlap(start_va, size_bytes, LPR_ZPOLINE_PAGE_VA, LPR_ZPOLINE_PAGE_SIZE)) {
        return LPR_LOW_LAYOUT_RESERVED;
    }
    if (lpr_ranges_overlap(start_va, size_bytes, LPR_LOW_GUARD_START_VA, LPR_LOW_GUARD_SIZE)) {
        return LPR_LOW_LAYOUT_RESERVED;
    }
    return LPR_LOW_LAYOUT_OK;
}

int lpr_low_region_allowed_for_linux_mapping(uint64_t start_va, uint64_t size_bytes) {
    const int reserved = lpr_low_region_is_reserved(start_va, size_bytes);
    if (reserved != LPR_LOW_LAYOUT_OK) return reserved;
    if (size_bytes != 0 && start_va < LPR_LOW_USER_MIN_VA) {
        return LPR_LOW_LAYOUT_RESERVED;
    }
    return LPR_LOW_LAYOUT_OK;
}
