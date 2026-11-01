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

/* Called for a candidate that some resynchronized decode path reaches as a
 * SYSCALL and another as interior bytes. Return nonzero only when the caller
 * has independent evidence that the candidate is data; it is then left
 * unmodified. Zero rejects the whole scan exactly as lpr_patch_scan_syscalls
 * does. The offset is relative to bytes. */
typedef int (*lpr_patch_scan_ambiguous_fn)(void *context, uint64_t offset);

int lpr_patch_scan_syscalls_resolved(uint8_t *bytes, uint64_t size,
                                     uint64_t flags,
                                     lpr_patch_scan_ambiguous_fn ambiguous,
                                     void *context,
                                     lpr_patch_scan_result_t *result);

#endif
