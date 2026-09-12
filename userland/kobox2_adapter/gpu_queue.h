/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_GPU_QUEUE_H
#define PACHA_KOBOX_GPU_QUEUE_H

#include "../gpud/gpu_channel.h"
#include "gpu_queue_message.h"
#include "gpu_session_service.h"

struct ph_gpu_queue {
    struct ph_gpu_session_service service;
    struct gpud_gpu_channel channel;
    kb2_vq_atomic_ops_t atomics;
    kb2_vq_segment_t segments[GPUD_GPU_QUEUE_SIZE];
    kb2_vq_chain_t request;
    void *mapping;
    uint64_t channel_id;
    unsigned int active_lane;
    unsigned int next_lane;
    int bound;
};

int ph_gpu_queue_init(struct ph_gpu_queue *queue,
                      struct gpud_gpu_sessions *sessions,
                      uint64_t client_id,
                      uint64_t channel_id,
                      const kb2_vq_atomic_ops_t *atomics,
                      struct ph_lifecycle_service *service);

#endif
