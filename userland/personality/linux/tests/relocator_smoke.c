#include <stdint.h>
#include <string.h>
#include "../loader/lpr_relocator.h"

struct test_rela {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t r_addend;
};

static int expect(int condition) {
    return condition ? 0 : 1;
}

int main(void) {
    uint8_t image[128];
    memset(image, 0, sizeof(image));

    struct test_rela rela[2] = {
        { .r_offset = 8, .r_info = 8, .r_addend = 0x30 },
        { .r_offset = 24, .r_info = 8, .r_addend = 0x48 },
    };
    memcpy(image + 64, rela, sizeof(rela));

    const struct lpr_relative_relocation_range range = {
        .rela_offset = 64,
        .rela_size = sizeof(rela),
        .rela_entry_size = sizeof(rela[0]),
    };
    int status = lpr_apply_relative_relocations(image, sizeof(image), 0x100000, &range);
    if (expect(status == LPR_RELOCATOR_OK)) return 1;
    if (expect(*(uint64_t *)(void *)(image + 8) == 0x100030)) return 1;
    if (expect(*(uint64_t *)(void *)(image + 24) == 0x100048)) return 1;

    const struct lpr_relative_relocation_range bad_range = {
        .rela_offset = 120,
        .rela_size = sizeof(rela),
        .rela_entry_size = sizeof(rela[0]),
    };
    status = lpr_apply_relative_relocations(image, sizeof(image), 0x100000, &bad_range);
    if (expect(status == LPR_RELOCATOR_BOUNDS)) return 1;
    return 0;
}
