#include <stdint.h>
#include <string.h>
#include <personality/linux_lpr.h>
#include <personality/zpoline.h>

void lpr_syscall_entry(void) {
}

static int expect(int condition) {
    return condition ? 0 : 1;
}

int main(void) {
    uint8_t code[] = {
        0x48, 0xc7, 0xc0, 0x27, 0x00, 0x00, 0x00,
        0x48, 0xb8, 0x11, 0x22, 0x0f, 0x05, 0x33, 0x44, 0x55, 0x66,
        0x0f, 0x05,
        0x90,
        0x0f, 0x05,
        0x90,
    };
    struct lpr_patch_mapping_result result;
    const struct lpr_patch_mapping_request request = {
        .start_va = (uint64_t)(uintptr_t)code,
        .size_bytes = sizeof(code),
        .flags = LPR_PATCH_FLAG_EXECUTABLE | LPR_PATCH_FLAG_PRIVATE,
    };
    if (expect(lpr_patch_mapping(&request, &result) == 0)) return 1;
    if (expect(result.patched_sites == 2)) return 1;
    if (expect(code[11] == LPR_ZPOLINE_PATCH_FROM0 && code[12] == LPR_ZPOLINE_PATCH_FROM1)) return 1;
    if (expect(code[17] == LPR_ZPOLINE_PATCH_TO0 && code[18] == LPR_ZPOLINE_PATCH_TO1)) return 1;
    if (expect(code[20] == LPR_ZPOLINE_PATCH_TO0 && code[21] == LPR_ZPOLINE_PATCH_TO1)) return 1;

    uint8_t page[LPR_ZPOLINE_PAGE_SIZE];
    memset(page, 0, sizeof(page));
    if (expect(lpr_build_zpoline_page(page, 0x1122334455667788ull) == 0)) return 1;
    const uint64_t common = lpr_zpoline_common_offset();
    if (expect(common == LPR_ZPOLINE_SHIM_OFFSET)) return 1;
    if (expect(page[0] == 0x90 && page[LPR_ZPOLINE_SHIM_OFFSET - 1] == 0x90)) return 1;
    if (expect(LPR_ZPOLINE_DIRECT_LIMIT == 512)) return 1;
    if (expect(page[common + 0] == 0x59 && page[common + 1] == 0x49 && page[common + 2] == 0xbb)) return 1;
    if (expect(page[common + 11] == 0x41 && page[common + 12] == 0xff && page[common + 13] == 0xe3)) return 1;
    return 0;
}
