#pragma once

#include <stdint.h>
#include <string.h>

enum {
    /* Linux 6.8 struct dma_fence::refcount, also asserted by Kobox's shim. */
    DRMD_DMA_FENCE_REFCOUNT_OFFSET = 0x38,
};

typedef struct drmd_flip_completion {
    int active;
    uint64_t owner;
    uint64_t user_data;
    uint32_t fb_id;
    void *fence;
} drmd_flip_completion_t;

static inline uint32_t drmd_dma_fence_refcount(const void *fence)
{
    uint32_t refs = 0;
    if (fence != NULL) {
        memcpy(&refs,
            (const uint8_t *)fence + DRMD_DMA_FENCE_REFCOUNT_OFFSET,
            sizeof(refs));
    }
    return refs;
}

static inline int drmd_flip_completion_begin(
    drmd_flip_completion_t *completion,
    uint64_t owner,
    uint64_t user_data,
    uint32_t fb_id,
    void *fence)
{
    if (completion == NULL) return -5;
    if (completion->active) return -16;
    if (fence == NULL || drmd_dma_fence_refcount(fence) != 2) return -5;
    completion->active = 1;
    completion->owner = owner;
    completion->user_data = user_data;
    completion->fb_id = fb_id;
    completion->fence = fence;
    return 0;
}

static inline int drmd_flip_completion_take(
    drmd_flip_completion_t *completion,
    drmd_flip_completion_t *out_completed)
{
    if (completion == NULL || out_completed == NULL ||
        !completion->active ||
        drmd_dma_fence_refcount(completion->fence) != 1) return 0;
    *out_completed = *completion;
    memset(completion, 0, sizeof(*completion));
    return 1;
}
