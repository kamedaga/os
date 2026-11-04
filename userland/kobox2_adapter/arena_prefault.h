/* SPDX-License-Identifier: MIT */
#ifndef PH_ARENA_PREFAULT_H
#define PH_ARENA_PREFAULT_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#ifndef PH_ARENA_LAZY
#define PH_ARENA_LAZY 0
#endif
/* Caller retains Linux page ownership and a stable index array through native
 * publication. In resident arena mode these selected image/DMA/fs_queue
 * reads are redundant: admission already populated every allocator page and
 * balloon retains discarded pages outside the allocator. Keep the stage1
 * range checks and compatibility with callers outside that mode; this helper
 * is never the fallible supply path. Call before masking notifications or
 * taking window/DMA/shared locks: a cold read may enter fault handling.
 * GPU cache lease callers separately assert resident admission before acquire.
 * Native OOM is fatal, not a recoverable ENOMEM or a successful prefault. */
static inline int ph_arena_prefault(const void *base, size_t bytes,
    const uint64_t *pages, size_t count, bool sparse) {
    if (!pages || !count || count > SIZE_MAX / sizeof(*pages) ||
        bytes % 4096 || (uintptr_t)base > UINTPTR_MAX - bytes) return 1;
    /* Validate the entire request before touching even its valid prefix. */
    for (size_t i = 0; i < count; ++i) {
        if (sparse && pages[i] == UINT64_MAX) continue;
        if (pages[i] >= bytes / 4096) return 1;
    }
#if PH_ARENA_LAZY
    if (!base) return 1;
    for (size_t i = 0; i < count; ++i) {
        if (sparse && pages[i] == UINT64_MAX) continue;
        /* Volatile forces a read without modifying shared/read-only data. */
        (void)*(const volatile unsigned char *)((uintptr_t)base + pages[i] * 4096);
    }
#endif
    return 0;
}
#endif
