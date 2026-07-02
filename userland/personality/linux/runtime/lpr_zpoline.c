#include <stdint.h>
#include <personality/linux_lpr.h>

#define LPR_X86_NOP 0x90u
#define LPR_X86_POP_RCX 0x59u
#define LPR_X86_MOVABS_R11_0 0x49u
#define LPR_X86_MOVABS_R11_1 0xbbu
#define LPR_X86_JMP_R11_0 0x41u
#define LPR_X86_JMP_R11_1 0xffu
#define LPR_X86_JMP_R11_2 0xe3u

int64_t lpr_build_zpoline_page(uint8_t *page, uint64_t handler_va) {
    if (page == 0 || handler_va == 0) {
        return -PERSONALITY_STATUS_INVALID;
    }

    for (uint64_t i = 0; i < LPR_ZPOLINE_PAGE_SIZE; i++) {
        page[i] = LPR_X86_NOP;
    }

    uint8_t *common = page + LPR_ZPOLINE_SHIM_OFFSET;
    common[0] = LPR_X86_POP_RCX;
    common[1] = LPR_X86_MOVABS_R11_0;
    common[2] = LPR_X86_MOVABS_R11_1;
    for (uint32_t i = 0; i < 8; i++) {
        common[3 + i] = (uint8_t)((handler_va >> (i * 8)) & 0xffu);
    }
    common[11] = LPR_X86_JMP_R11_0;
    common[12] = LPR_X86_JMP_R11_1;
    common[13] = LPR_X86_JMP_R11_2;
    return PERSONALITY_STATUS_OK;
}

uint64_t lpr_zpoline_common_offset(void) {
    return LPR_ZPOLINE_SHIM_OFFSET;
}
