#include "lpr_relocator.h"

#define LPR_R_X86_64_RELATIVE 8ull

struct lpr_elf64_rela {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t r_addend;
};

static uint32_t lpr_relocation_type(uint64_t info) {
    return (uint32_t)(info & 0xffffffffu);
}

static int lpr_range_overflows(uint64_t offset, uint64_t size, uint64_t limit) {
    return offset > limit || size > limit - offset;
}

int lpr_apply_relative_relocations(uint8_t *image,
                                   uint64_t image_size,
                                   uint64_t load_bias,
                                   const struct lpr_relative_relocation_range *range) {
    if (image == 0 || range == 0) {
        return LPR_RELOCATOR_INVALID;
    }
    if (range->rela_entry_size != sizeof(struct lpr_elf64_rela)) {
        return LPR_RELOCATOR_UNSUPPORTED;
    }
    if (range->rela_size % range->rela_entry_size != 0) {
        return LPR_RELOCATOR_INVALID;
    }
    if (lpr_range_overflows(range->rela_offset, range->rela_size, image_size)) {
        return LPR_RELOCATOR_BOUNDS;
    }

    const uint64_t count = range->rela_size / range->rela_entry_size;
    const struct lpr_elf64_rela *rela =
        (const struct lpr_elf64_rela *)(const void *)(image + range->rela_offset);
    for (uint64_t i = 0; i < count; i++) {
        if (lpr_relocation_type(rela[i].r_info) != LPR_R_X86_64_RELATIVE) {
            return LPR_RELOCATOR_UNSUPPORTED;
        }
        if (lpr_range_overflows(rela[i].r_offset, sizeof(uint64_t), image_size)) {
            return LPR_RELOCATOR_BOUNDS;
        }
        uint64_t *target = (uint64_t *)(void *)(image + rela[i].r_offset);
        *target = load_bias + (uint64_t)rela[i].r_addend;
    }
    return LPR_RELOCATOR_OK;
}
