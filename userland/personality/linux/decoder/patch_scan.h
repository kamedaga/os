#ifndef PACHA_LPR_PATCH_SCAN_H
#define PACHA_LPR_PATCH_SCAN_H

#include <stdint.h>

typedef struct lpr_patch_scan_result {
    uint64_t patched_sites;
    uint64_t skipped_sites;
    uint64_t failed_sites;
} lpr_patch_scan_result_t;

enum lpr_patch_scan_flags {
    /* The first byte is an instruction boundary and the complete range is a
     * contiguous instruction stream (for example an ELF .text section). */
    LPR_PATCH_SCAN_START_BOUNDARY = 1u << 0,
};

int lpr_patch_scan_syscalls(uint8_t *bytes, uint64_t size, uint64_t flags,
                            lpr_patch_scan_result_t *result);

#endif
