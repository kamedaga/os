#ifndef LPR_LOW_LAYOUT_H
#define LPR_LOW_LAYOUT_H

#include <stdint.h>
#include <personality/zpoline.h>

enum lpr_low_layout_status {
    LPR_LOW_LAYOUT_OK = 0,
    LPR_LOW_LAYOUT_INVALID = 1,
    LPR_LOW_LAYOUT_RESERVED = 2,
    LPR_LOW_LAYOUT_OVERFLOW = 3,
};

enum lpr_low_mapping_kind {
    LPR_LOW_MAPPING_ZPOLINE = 1,
    LPR_LOW_MAPPING_GUARD = 2,
};

enum lpr_low_mapping_prot {
    LPR_LOW_PROT_NONE = 0,
    LPR_LOW_PROT_READ = 1u << 0,
    LPR_LOW_PROT_WRITE = 1u << 1,
    LPR_LOW_PROT_EXEC = 1u << 2,
};

struct lpr_low_mapping {
    uint64_t start_va;
    uint64_t size_bytes;
    uint32_t prot;
    uint32_t kind;
};

typedef int (*lpr_low_layout_map_fn)(void *context, const struct lpr_low_mapping *mapping);

#define LPR_LOW_LAYOUT_MAPPING_COUNT 2u

void lpr_low_layout_plan(struct lpr_low_mapping out[LPR_LOW_LAYOUT_MAPPING_COUNT]);
int lpr_low_layout_install(void *context, lpr_low_layout_map_fn map_fixed);
int lpr_low_region_is_reserved(uint64_t start_va, uint64_t size_bytes);
int lpr_low_region_allowed_for_linux_mapping(uint64_t start_va, uint64_t size_bytes);

#endif
