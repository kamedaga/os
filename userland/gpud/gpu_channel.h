/* SPDX-License-Identifier: MIT */
#ifndef PACHA_GPUD_GPU_CHANNEL_H
#define PACHA_GPUD_GPU_CHANNEL_H

#include "gpu_limits.h"
#include <kobox2/gpu_drm_virtgpu_layout.h>
#include <kobox2/virtqueue_memory.h>

enum {
    GPUD_GPU_QUEUE_EVENT,
    GPUD_GPU_QUEUE_CONTROL,
    GPUD_GPU_QUEUE_DISPLAY,
    GPUD_GPU_QUEUE_EXECUTION,
    GPUD_GPU_QUEUE_COUNT,
    GPUD_GPU_QUEUE_SIZE = 16,
    GPUD_GPU_CHANNEL_PAGE = 4096,
    GPUD_GPU_EVENT_OFFSET = GPUD_GPU_CHANNEL_PAGE,
    GPUD_GPU_CONTROL_REQUEST_OFFSET = 2 * GPUD_GPU_CHANNEL_PAGE,
    GPUD_GPU_CONTROL_REPLY_OFFSET = 2 * GPUD_GPU_CHANNEL_PAGE + 2048,
    GPUD_GPU_CONTROL_REPLY_CAPACITY = 1024,
    GPUD_GPU_DISPLAY_REQUEST_OFFSET = 3 * GPUD_GPU_CHANNEL_PAGE,
    GPUD_GPU_DISPLAY_REPLY_OFFSET = 3 * GPUD_GPU_CHANNEL_PAGE + 2048,
    GPUD_GPU_DISPLAY_REPLY_CAPACITY = 1024,
    GPUD_GPU_REQUEST_OFFSET = 4 * GPUD_GPU_CHANNEL_PAGE,
    GPUD_GPU_OUTPUT_OFFSET = 5 * GPUD_GPU_CHANNEL_PAGE,
    GPUD_GPU_REPLY_OFFSET = 6 * GPUD_GPU_CHANNEL_PAGE,
    GPUD_GPU_AUX_OFFSET = 7 * GPUD_GPU_CHANNEL_PAGE,
    GPUD_GPU_AUX_CAPACITY = KB2_GPU_DRM_VIRTGPU_MAX_COMMAND_BYTES +
        KB2_GPU_DRM_VIRTGPU_MAX_BO_HANDLES * 4,
    GPUD_GPU_CHANNEL_SIZE = GPUD_GPU_AUX_OFFSET + GPUD_GPU_AUX_CAPACITY,
};

/* MIT host policy shared by the native gpud frontend and GPL sandbox adapter.
 * No controller or Linux object layouts are linked across that boundary.
 * These arrays are private, authenticated launch policy, never peer choices.
 */
struct gpud_gpu_channel {
    kb2_protocol_channel_t identity;
    kb2_protocol_queue_t queues[GPUD_GPU_QUEUE_COUNT];
    kb2_protocol_region_t region;
    kb2_vq_channel_t channel;
    kb2_vq_mapping_t mapping;
    kb2_vq_arena_t arenas[GPUD_GPU_QUEUE_COUNT];
    kb2_vq_mapped_memory_t memory;
    kb2_vq_t lanes[GPUD_GPU_QUEUE_COUNT];
    kb2_vq_chain_t *slots[GPUD_GPU_QUEUE_COUNT][GPUD_GPU_QUEUE_SIZE];
    uint16_t owners[GPUD_GPU_QUEUE_COUNT][GPUD_GPU_QUEUE_SIZE];
};

/* Fresh zeroed VMO and uninitialized private state only. DRIVER writes the
 * channel header before capability transfer; DEVICE snapshots it and compares
 * against the independently trusted generation/channel profile. No reset API.
 * The caller pins the mapping through all lane users and terminal teardown.
 */
int ph_gpu_channel_bind(struct gpud_gpu_channel *channel,
                        void *mapping,
                        uint64_t generation,
                        uint64_t channel_id,
                        kb2_vq_side_t side,
                        const kb2_vq_atomic_ops_t *atomics);

#endif
