#ifndef PERSONALITY_ZPOLINE_H
#define PERSONALITY_ZPOLINE_H

#include <stdint.h>
#include "lpr_image_abi.h"

enum lpr_patch_flags {
    LPR_PATCH_FLAG_EXECUTABLE = 1u << 0,
    LPR_PATCH_FLAG_PRIVATE = 1u << 1,
    LPR_PATCH_FLAG_VALIDATED_INSN = 1u << 2,
};

enum lpr_patch_status {
    LPR_PATCH_STATUS_OK = 0,
    LPR_PATCH_STATUS_NOT_EXECUTABLE = 1,
    LPR_PATCH_STATUS_NOT_PRIVATE = 2,
    LPR_PATCH_STATUS_DECODE_FAILED = 3,
    LPR_PATCH_STATUS_NO_SYSCALL = 4,
};

struct lpr_patch_mapping_request {
    uint64_t start_va;
    uint64_t size_bytes;
    uint64_t flags;
};

struct lpr_patch_mapping_result {
    uint64_t scanned_bytes;
    uint64_t patched_sites;
    uint64_t skipped_sites;
    uint64_t failed_sites;
    uint64_t cycles;
};

_Static_assert(sizeof(struct lpr_patch_mapping_request) == 24, "lpr_patch_mapping_request size");
_Static_assert(sizeof(struct lpr_patch_mapping_result) == 40, "lpr_patch_mapping_result size");
_Static_assert(LPR_LOW_GUARD_START_VA == PERSONALITY_PAGE_SIZE, "zpoline page must be first");
_Static_assert((LPR_LOW_GUARD_END_VA & (PERSONALITY_PAGE_SIZE - 1)) == 0, "low guard end page aligned");
_Static_assert(LPR_LOW_GUARD_SIZE >= PERSONALITY_PAGE_SIZE, "low guard must contain at least one page");
_Static_assert(LPR_LOW_GUARD_END_VA <= LPR_LINUX_ET_EXEC_BASE_VA, "low guard must not reserve normal ET_EXEC base");
_Static_assert(LPR_ZPOLINE_SHIM_OFFSET == 512, "zpoline shim should keep normal Linux syscall numbers short");
_Static_assert(LPR_ZPOLINE_SHIM_OFFSET + LPR_ZPOLINE_SHIM_SIZE < LPR_ZPOLINE_PAGE_SIZE, "zpoline shim must fit in the low page");

int64_t lpr_patch_mapping(const struct lpr_patch_mapping_request *request,
                          struct lpr_patch_mapping_result *result);
int64_t lpr_init_zpoline_page(uint8_t *page);
int64_t lpr_build_zpoline_page(uint8_t *page, uint64_t handler_va);
uint64_t lpr_zpoline_common_offset(void);

#endif
