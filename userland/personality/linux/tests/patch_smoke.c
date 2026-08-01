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
        /* VEX2 vmovups with 0f 05 inside its displacement. */
        0xc5, 0xfc, 0x10, 0x84, 0x24, 0x0f, 0x05, 0x00, 0x00,
        0x0f, 0x05,
        0x90,
        /* VEX3 vpbroadcastd. */
        0xc4, 0xe2, 0x7d, 0x58, 0xc1,
        0x0f, 0x05,
        0x90,
        /* EVEX vaddps. Decoding must not depend on host AVX-512 support. */
        0x62, 0xf1, 0x7c, 0x48, 0x58, 0xc1,
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
    if (expect(result.patched_sites == 5)) return 1;
    if (expect(code[11] == LPR_ZPOLINE_PATCH_FROM0 && code[12] == LPR_ZPOLINE_PATCH_FROM1)) return 1;
    if (expect(code[17] == LPR_ZPOLINE_PATCH_TO0 && code[18] == LPR_ZPOLINE_PATCH_TO1)) return 1;
    if (expect(code[20] == LPR_ZPOLINE_PATCH_TO0 && code[21] == LPR_ZPOLINE_PATCH_TO1)) return 1;
    if (expect(code[28] == LPR_ZPOLINE_PATCH_FROM0 && code[29] == LPR_ZPOLINE_PATCH_FROM1)) return 1;
    if (expect(code[32] == LPR_ZPOLINE_PATCH_TO0 && code[33] == LPR_ZPOLINE_PATCH_TO1)) return 1;
    if (expect(code[40] == LPR_ZPOLINE_PATCH_TO0 && code[41] == LPR_ZPOLINE_PATCH_TO1)) return 1;
    if (expect(code[49] == LPR_ZPOLINE_PATCH_TO0 && code[50] == LPR_ZPOLINE_PATCH_TO1)) return 1;

    /* The x86-64 zpoline ABI replaces SYSCALL only; do not broaden it to
     * legacy SYSENTER merely because the decoder recognizes that mnemonic. */
    uint8_t sysenter_code[] = { 0x0f, 0x34, 0x90 };
    const struct lpr_patch_mapping_request sysenter_request = {
        .start_va = (uint64_t)(uintptr_t)sysenter_code,
        .size_bytes = sizeof(sysenter_code),
        .flags = LPR_PATCH_FLAG_EXECUTABLE | LPR_PATCH_FLAG_PRIVATE,
    };
    if (expect(lpr_patch_mapping(&sysenter_request, &result) == 0)) return 1;
    if (expect(result.patched_sites == 0)) return 1;
    if (expect(sysenter_code[0] == 0x0f && sysenter_code[1] == 0x34)) return 1;

    uint8_t shared_code[] = { 0x0f, 0x05, 0x90 };
    const struct lpr_patch_mapping_request shared_request = {
        .start_va = (uint64_t)(uintptr_t)shared_code,
        .size_bytes = sizeof(shared_code),
        .flags = LPR_PATCH_FLAG_EXECUTABLE,
    };
    if (expect(lpr_patch_mapping(&shared_request, &result) ==
               LPR_PATCH_STATUS_NOT_PRIVATE)) return 1;
    if (expect(shared_code[0] == 0x0f && shared_code[1] == 0x05)) return 1;

    /* A verified-boundary scan is transactional: a later undecodable byte
     * must not leave an earlier syscall rewritten. */
    uint8_t invalid_code[] = {
        0x0f, 0x05, 0x90,
        0x0f, 0x04,
        0x0f, 0x05, 0x90,
    };
    const struct lpr_patch_mapping_request invalid_request = {
        .start_va = (uint64_t)(uintptr_t)invalid_code,
        .size_bytes = sizeof(invalid_code),
        .flags = LPR_PATCH_FLAG_EXECUTABLE | LPR_PATCH_FLAG_PRIVATE |
                 LPR_PATCH_FLAG_VALIDATED_INSN,
    };
    if (expect(lpr_patch_mapping(&invalid_request, &result) < 0)) return 1;
    if (expect(result.failed_sites == 1)) return 1;
    if (expect(invalid_code[0] == 0x0f && invalid_code[1] == 0x05)) return 1;

    uint8_t far_code[512];
    memset(far_code, 0x90, sizeof(far_code));
    far_code[300] = 0x48;
    far_code[301] = 0xb8;
    far_code[304] = 0x0f;
    far_code[305] = 0x05;
    far_code[340] = 0x0f;
    far_code[341] = 0x05;
    far_code[380] = 0x66;
    far_code[381] = 0x0f;
    far_code[382] = 0x05;
    const struct lpr_patch_mapping_request far_request = {
        .start_va = (uint64_t)(uintptr_t)far_code,
        .size_bytes = sizeof(far_code),
        .flags = LPR_PATCH_FLAG_EXECUTABLE | LPR_PATCH_FLAG_PRIVATE,
    };
    if (expect(lpr_patch_mapping(&far_request, &result) == 0)) return 1;
    if (expect(result.patched_sites == 2)) return 1;
    if (expect(far_code[304] == LPR_ZPOLINE_PATCH_FROM0 &&
               far_code[305] == LPR_ZPOLINE_PATCH_FROM1)) return 1;
    if (expect(far_code[340] == LPR_ZPOLINE_PATCH_TO0 &&
               far_code[341] == LPR_ZPOLINE_PATCH_TO1)) return 1;
    if (expect(far_code[380] == 0x66 &&
               far_code[381] == LPR_ZPOLINE_PATCH_TO0 &&
               far_code[382] == LPR_ZPOLINE_PATCH_TO1)) return 1;

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
