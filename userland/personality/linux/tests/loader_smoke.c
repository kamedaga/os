#include <stdint.h>
#include <string.h>
#include "../loader/lpr_loader.h"

struct map_trace {
    unsigned count;
    struct lpr_loader_mapping_image images[LPR_LOW_LAYOUT_MAPPING_COUNT];
    uint8_t zpoline_copy[LPR_ZPOLINE_PAGE_SIZE];
};

static int expect(int condition) {
    return condition ? 0 : 1;
}

static int trace_map_fixed(void *context, const struct lpr_loader_mapping_image *image) {
    struct map_trace *trace = (struct map_trace *)context;
    if (trace == 0 || image == 0 || trace->count >= LPR_LOW_LAYOUT_MAPPING_COUNT) {
        return -1;
    }
    trace->images[trace->count] = *image;
    if (image->mapping.kind == LPR_LOW_MAPPING_ZPOLINE) {
        if (image->initial_bytes == 0 || image->initial_size != LPR_ZPOLINE_PAGE_SIZE) {
            return -1;
        }
        memcpy(trace->zpoline_copy, image->initial_bytes, LPR_ZPOLINE_PAGE_SIZE);
        trace->images[trace->count].initial_bytes = trace->zpoline_copy;
    }
    trace->count++;
    return 0;
}

void lpr_syscall_entry(void) {
}

int main(void) {
    struct map_trace trace;
    memset(&trace, 0, sizeof(trace));
    const struct lpr_loader_low_ops ops = {
        .map_fixed = trace_map_fixed,
    };

    if (expect(lpr_loader_install_low_layout(&trace, &ops, 0x1122334455667788ull) == LPR_LOADER_OK)) return 1;
    if (expect(trace.count == LPR_LOW_LAYOUT_MAPPING_COUNT)) return 1;
    if (expect(trace.images[0].mapping.kind == LPR_LOW_MAPPING_ZPOLINE)) return 1;
    if (expect(trace.images[0].mapping.start_va == LPR_ZPOLINE_PAGE_VA)) return 1;
    if (expect(trace.images[0].mapping.prot == (LPR_LOW_PROT_READ | LPR_LOW_PROT_EXEC))) return 1;
    if (expect(trace.images[1].mapping.kind == LPR_LOW_MAPPING_GUARD)) return 1;
    if (expect(trace.images[1].mapping.prot == LPR_LOW_PROT_NONE)) return 1;
    if (expect(trace.images[1].initial_bytes == 0 && trace.images[1].initial_size == 0)) return 1;

    const uint64_t shim = lpr_zpoline_common_offset();
    if (expect(trace.zpoline_copy[0] == 0x90)) return 1;
    if (expect(trace.zpoline_copy[shim + 0] == 0x59 && trace.zpoline_copy[shim + 1] == 0x49 && trace.zpoline_copy[shim + 2] == 0xbb)) return 1;
    if (expect(trace.zpoline_copy[shim + 11] == 0x41 && trace.zpoline_copy[shim + 12] == 0xff && trace.zpoline_copy[shim + 13] == 0xe3)) return 1;

    uint8_t code[] = {0x90, 0x0f, 0x05, 0x90};
    struct lpr_patch_mapping_result patch;
    if (expect(lpr_loader_patch_executable_mapping(code, sizeof(code), 1, &patch) == LPR_LOADER_OK)) return 1;
    if (expect(patch.patched_sites == 1)) return 1;
    if (expect(code[1] == LPR_ZPOLINE_PATCH_TO0 && code[2] == LPR_ZPOLINE_PATCH_TO1)) return 1;

    uint8_t data[] = {0x0f, 0x05};
    if (expect(lpr_loader_patch_executable_mapping(data, sizeof(data), 0, &patch) == LPR_LOADER_OK)) return 1;
    if (expect(patch.patched_sites == 0)) return 1;
    if (expect(data[0] == 0x0f && data[1] == 0x05)) return 1;
    return 0;
}
