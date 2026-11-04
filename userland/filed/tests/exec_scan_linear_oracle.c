#include "internal.h"
#include <patch_scan.h>
#include "filed/code_ranges.h"
#include <stdlib.h>
#include <string.h>
/* Pre-mc3 linear-first oracle: retain acceptance/patch/failure semantics. */
struct exec_code_reader { const lpr_exec_image_t *image; };

static int exec_code_read(void *raw, uint64_t file_offset,
                          uint8_t *destination, size_t length)
{
    const struct exec_code_reader *reader = raw;
    if (file_offset > reader->image->size ||
        length > reader->image->size - file_offset) return -5;
    memcpy(destination, reader->image->bytes + file_offset, length);
    return 0;
}

struct exec_ambiguous_context {
    const filed_rx_code_ranges_t *code;
    uint64_t base;
};

static int exec_candidate_outside_code(void *raw, uint64_t offset)
{
    const struct exec_ambiguous_context *context = raw;
    if (offset > UINT64_MAX - context->base) return 0;
    return filed_rx_code_ranges_outside(context->code, context->base + offset);
}

int mc3_linear_first_oracle(
    const lpr_exec_image_t *image,
    uint64_t file_offset,
    unsigned char *bytes,
    uint64_t size,
    uint64_t *out_patched)
{
    if (image == NULL || bytes == NULL || out_patched == NULL) {
        return -22;
    }
    *out_patched = 0;
    if (size > SIZE_MAX) return -75;
    if (size == 0) return 0;
    unsigned char *original = malloc((size_t)size);
    if (original == NULL) return -12;
    memcpy(original, bytes, (size_t)size);
    lpr_patch_scan_result_t result;
    int status = lpr_patch_scan_syscalls(
        bytes, size, LPR_PATCH_SCAN_START_BOUNDARY, &result);
    if (status != 0) {
        /* Match executable snapshot manifests: an executable section may
         * contain assembler data, so retry the conservative candidate scan
         * on immutable input. Ambiguous syscall candidates still reject. */
        memcpy(bytes, original, (size_t)size);
        status = lpr_patch_scan_syscalls(bytes, size, 0, &result);
    }
    if (status != 0) {
        /* As for RX manifests: only a candidate outside every .eh_frame FDE
         * may be treated as data. Images without FDEs keep rejecting. */
        struct exec_code_reader reader = { image };
        filed_rx_code_ranges_t code;
        if (filed_rx_code_ranges_load(exec_code_read, &reader, image->size,
                                      &code) == 0) {
            struct exec_ambiguous_context context = { &code, file_offset };
            memcpy(bytes, original, (size_t)size);
            status = lpr_patch_scan_syscalls_resolved(
                bytes, size, 0, exec_candidate_outside_code, &context, &result);
            filed_rx_code_ranges_free(&code);
        }
    }
    if (status != 0) memcpy(bytes, original, (size_t)size);
    free(original);
    if (status != 0) return -8;
    *out_patched = result.patched_sites;
    return 0;
}
