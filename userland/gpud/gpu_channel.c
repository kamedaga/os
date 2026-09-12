/* SPDX-License-Identifier: MIT */
#include "gpu_channel.h"

#include <errno.h>
#include <kobox2/gpu.h>
#include <string.h>

int ph_gpu_channel_bind(struct gpud_gpu_channel *channel,
                        void *mapping,
                        uint64_t generation,
                        uint64_t channel_id,
                        kb2_vq_side_t side,
                        const kb2_vq_atomic_ops_t *atomics) {
    if (!channel || channel->identity.generation || !mapping || !generation || !channel_id ||
        !atomics || (side != KB2_VQ_DRIVER && side != KB2_VQ_DEVICE))
        return -EINVAL;
    channel->identity =
        (kb2_protocol_channel_t){.feature_bits = KB2_PROTOCOL_TRANSPORT_FEATURES_REQUIRED,
                                 .channel_id = channel_id,
                                 .generation = generation,
                                 .protocol_id = KB2_GPU_PROTOCOL_ID,
                                 .flags = KB2_PROTOCOL_CHANNEL_FLAG_DATA};
    for (unsigned int i = 0; i < GPUD_GPU_QUEUE_COUNT; ++i) {
        channel->queues[i] = (kb2_protocol_queue_t){
            .queue_id = i + 1,
            .role = i ? KB2_PROTOCOL_QUEUE_ROLE_REQUEST : KB2_PROTOCOL_QUEUE_ROLE_EVENT,
            .queue_size = GPUD_GPU_QUEUE_SIZE,
            .descriptor_address = 512 + i * 512,
            .available_address = 768 + i * 512,
            .used_address = 808 + i * 512,
            .available_notification_id = 2 * i + 1,
            .used_notification_id = 2 * i + 2,
            .max_chain_length = GPUD_GPU_QUEUE_SIZE,
            .max_indirect_length = 32,
            /* This query profile has one pre-bound GPU output page. Its
             * ownership must extend through the frontend's completion copy. */
            .max_outstanding = 1};
        channel->arenas[i] = (kb2_vq_arena_t){
            .queue_id = i + 1,
            .rights = KB2_PROTOCOL_REGION_RIGHT_READ | KB2_PROTOCOL_REGION_RIGHT_WRITE,
            .transport_base = (i + 1) * GPUD_GPU_CHANNEL_PAGE,
            .length = (i == GPUD_GPU_QUEUE_EXECUTION ? 3 : 1) * GPUD_GPU_CHANNEL_PAGE};
    }
    channel->region = (kb2_protocol_region_t){.region_id = 1,
                                              .rights = KB2_PROTOCOL_REGION_RIGHT_READ |
                                                        KB2_PROTOCOL_REGION_RIGHT_WRITE,
                                              .length = GPUD_GPU_CHANNEL_SIZE};
    channel->mapping = (kb2_vq_mapping_t){.address = mapping,
                                          .length = GPUD_GPU_CHANNEL_SIZE,
                                          .local_rights = channel->region.rights};
    unsigned char expected[KB2_PROTOCOL_CHANNEL_HEADER_SIZE +
                           GPUD_GPU_QUEUE_COUNT * KB2_PROTOCOL_QUEUE_DESCRIPTOR_SIZE +
                           KB2_PROTOCOL_REGION_DESCRIPTOR_SIZE];
    unsigned char snapshot[sizeof(expected)];
    size_t size;
    if (kb2_protocol_channel_encode(expected,
                                    sizeof(expected),
                                    &size,
                                    &channel->identity,
                                    channel->queues,
                                    GPUD_GPU_QUEUE_COUNT,
                                    &channel->region,
                                    1) ||
        size != sizeof(expected))
        return -EPROTO;
    if (side == KB2_VQ_DRIVER) {
        memcpy(mapping, expected, size);
    } else {
        memcpy(snapshot, mapping, size);
        if (memcmp(snapshot, expected, size))
            return -EPROTO;
    }
    if (kb2_vq_mapped_memory_init(&channel->memory,
                                  &channel->mapping,
                                  1,
                                  channel->arenas,
                                  GPUD_GPU_QUEUE_COUNT,
                                  atomics) ||
        kb2_vq_channel_init(&channel->channel,
                            &channel->identity,
                            channel->queues,
                            GPUD_GPU_QUEUE_COUNT,
                            &channel->region,
                            1))
        return -EPROTO;
    kb2_vq_memory_t memory = kb2_vq_mapped_memory_ops(&channel->memory);
    for (unsigned int i = 0; i < GPUD_GPU_QUEUE_COUNT; ++i) {
        if (kb2_vq_bind(&channel->lanes[i],
                        &channel->channel,
                        i + 1,
                        side,
                        &memory,
                        channel->slots[i],
                        channel->owners[i],
                        GPUD_GPU_QUEUE_SIZE))
            return -EPROTO;
    }
    return 0;
}
