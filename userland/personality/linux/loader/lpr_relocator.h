#ifndef LPR_RELOCATOR_H
#define LPR_RELOCATOR_H

#include <stdint.h>

enum lpr_relocator_status {
    LPR_RELOCATOR_OK = 0,
    LPR_RELOCATOR_INVALID = 1,
    LPR_RELOCATOR_UNSUPPORTED = 2,
    LPR_RELOCATOR_BOUNDS = 3,
};

struct lpr_relative_relocation_range {
    uint64_t rela_offset;
    uint64_t rela_size;
    uint64_t rela_entry_size;
};

int lpr_apply_relative_relocations(uint8_t *image,
                                   uint64_t image_size,
                                   uint64_t load_bias,
                                   const struct lpr_relative_relocation_range *range);

#endif
