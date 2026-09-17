/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_GPU_QUEUE_H
#define PACHA_KOBOX_GPU_QUEUE_H

#include "../gpud/gpu_channel.h"
#include "gpu_event_message.h"
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
    uint64_t release_mapping_id;
    uint64_t release_correlation;
    struct kobox_linux_drm_event_host event_host;
    atomic_uint event_admitted;
    atomic_uint_fast64_t event_pending;
    atomic_uint_fast64_t event_cookies[GPUD_GPU_NATIVE_SESSION_LIMIT];
    atomic_int event_error;
    uint64_t event_cookie, event_session, event_sequence;
    size_t event_size, event_message_size;
    unsigned char event_data[PH_GPU_DRM_EVENT_BYTES];
    unsigned char event_message[PH_GPU_DRM_EVENT_MESSAGE_BYTES];
    unsigned int active_lane;
    unsigned int next_lane;
    int event_fd;
    int bound, event_active;
};

int ph_gpu_queue_init(struct ph_gpu_queue *queue,
                      struct gpud_gpu_sessions *sessions,
                      uint64_t client_id,
                      uint64_t channel_id,
                      const kb2_vq_atomic_ops_t *atomics,
                      struct ph_lifecycle_service *service);
/* After Linux device quiesce removed every DRM waitqueue observer and the
 * lifecycle receiver joined. */
int ph_gpu_queue_finish(struct ph_gpu_queue *queue);

#endif
