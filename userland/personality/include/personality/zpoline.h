#ifndef PERSONALITY_ZPOLINE_H
#define PERSONALITY_ZPOLINE_H

#include <stdint.h>
#include "personality_abi.h"

#define LPR_ZPOLINE_PAGE_VA 0ull
#define LPR_ZPOLINE_PAGE_SIZE PERSONALITY_PAGE_SIZE
#define LPR_LOW_GUARD_START_VA (LPR_ZPOLINE_PAGE_VA + LPR_ZPOLINE_PAGE_SIZE)
#define LPR_LOW_GUARD_END_VA 0x00100000ull
#define LPR_LOW_GUARD_SIZE (LPR_LOW_GUARD_END_VA - LPR_LOW_GUARD_START_VA)
#define LPR_LOW_USER_MIN_VA LPR_LOW_GUARD_END_VA
#define LPR_LINUX_ET_EXEC_BASE_VA 0x00400000ull
#define LPR_ZPOLINE_PATCH_FROM0 0x0fu
#define LPR_ZPOLINE_PATCH_FROM1 0x05u
#define LPR_ZPOLINE_PATCH_TO0 0xffu
#define LPR_ZPOLINE_PATCH_TO1 0xd0u
#define LPR_ZPOLINE_SHIM_SIZE 13u
#define LPR_ZPOLINE_SHIM_OFFSET 512u
#define LPR_ZPOLINE_DIRECT_LIMIT LPR_ZPOLINE_SHIM_OFFSET
#define LPR_ZPOLINE_MAX_SYSCALL_NR LPR_ZPOLINE_DIRECT_LIMIT

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

#endif
