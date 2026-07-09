#include <stdint.h>
#include <personality/linux_lpr.h>
#include <personality/zpoline.h>

int64_t lpr_build_zpoline_page(uint8_t *page, uint64_t handler_va) {
    if (page == 0 || handler_va == 0) {
        return -PERSONALITY_STATUS_INVALID;
    }

    for (uint64_t i = 0; i < LPR_ZPOLINE_PAGE_SIZE; i++) {
        page[i] = LPR_ZPOLINE_NOP_BYTE;
    }

    uint8_t *common = page + LPR_ZPOLINE_SHIM_OFFSET;
    common[LPR_ZPOLINE_SHIM_POP_RCX_OFFSET] = LPR_ZPOLINE_SHIM_POP_RCX_BYTE;
    common[LPR_ZPOLINE_SHIM_MOVABS_R11_OFFSET] = LPR_ZPOLINE_SHIM_MOVABS_R11_BYTE0;
    common[LPR_ZPOLINE_SHIM_MOVABS_R11_OFFSET + 1u] = LPR_ZPOLINE_SHIM_MOVABS_R11_BYTE1;
    for (uint32_t i = 0; i < LPR_ZPOLINE_SHIM_HANDLER_VA_SIZE; i++) {
        common[LPR_ZPOLINE_SHIM_HANDLER_VA_OFFSET + i] =
            (uint8_t)((handler_va >> (i * 8u)) & 0xffu);
    }
    common[LPR_ZPOLINE_SHIM_JMP_R11_OFFSET] = LPR_ZPOLINE_SHIM_JMP_R11_BYTE0;
    common[LPR_ZPOLINE_SHIM_JMP_R11_OFFSET + 1u] = LPR_ZPOLINE_SHIM_JMP_R11_BYTE1;
    common[LPR_ZPOLINE_SHIM_JMP_R11_OFFSET + 2u] = LPR_ZPOLINE_SHIM_JMP_R11_BYTE2;
    return PERSONALITY_STATUS_OK;
}

uint64_t lpr_zpoline_common_offset(void) {
    return LPR_ZPOLINE_SHIM_OFFSET;
}
