#include "../loader/lpr_low_layout.h"

struct install_trace {
    unsigned count;
    struct lpr_low_mapping mappings[LPR_LOW_LAYOUT_MAPPING_COUNT];
};

static int trace_map(void *context, const struct lpr_low_mapping *mapping) {
    struct install_trace *trace = (struct install_trace *)context;
    if (trace == 0 || mapping == 0 || trace->count >= LPR_LOW_LAYOUT_MAPPING_COUNT) {
        return LPR_LOW_LAYOUT_INVALID;
    }
    trace->mappings[trace->count++] = *mapping;
    return LPR_LOW_LAYOUT_OK;
}

static int expect(int condition) {
    return condition ? 0 : 1;
}

int main(void) {
    struct lpr_low_mapping plan[LPR_LOW_LAYOUT_MAPPING_COUNT];
    lpr_low_layout_plan(plan);

    if (expect(plan[0].kind == LPR_LOW_MAPPING_ZPOLINE)) return 1;
    if (expect(plan[0].start_va == LPR_ZPOLINE_PAGE_VA)) return 1;
    if (expect(plan[0].size_bytes == LPR_ZPOLINE_PAGE_SIZE)) return 1;
    if (expect((plan[0].prot & LPR_LOW_PROT_EXEC) != 0)) return 1;

    if (expect(plan[1].kind == LPR_LOW_MAPPING_GUARD)) return 1;
    if (expect(plan[1].start_va == LPR_LOW_GUARD_START_VA)) return 1;
    if (expect(plan[1].size_bytes == LPR_LOW_GUARD_SIZE)) return 1;
    if (expect(plan[1].prot == LPR_LOW_PROT_NONE)) return 1;

    if (expect(lpr_low_region_allowed_for_linux_mapping(0, 1) == LPR_LOW_LAYOUT_RESERVED)) return 1;
    if (expect(lpr_low_region_allowed_for_linux_mapping(0x1000, 0x1000) == LPR_LOW_LAYOUT_RESERVED)) return 1;
    if (expect(lpr_low_region_allowed_for_linux_mapping(0x400000, 0x1000) == LPR_LOW_LAYOUT_OK)) return 1;
    if (expect(lpr_low_region_allowed_for_linux_mapping(LPR_LOW_USER_MIN_VA, 0x1000) == LPR_LOW_LAYOUT_OK)) return 1;
    if (expect(lpr_low_region_allowed_for_linux_mapping(UINT64_MAX, 2) == LPR_LOW_LAYOUT_OVERFLOW)) return 1;

    struct install_trace trace = {0};
    if (expect(lpr_low_layout_install(&trace, trace_map) == LPR_LOW_LAYOUT_OK)) return 1;
    if (expect(trace.count == LPR_LOW_LAYOUT_MAPPING_COUNT)) return 1;
    if (expect(trace.mappings[0].kind == LPR_LOW_MAPPING_ZPOLINE)) return 1;
    if (expect(trace.mappings[1].kind == LPR_LOW_MAPPING_GUARD)) return 1;
    return 0;
}
