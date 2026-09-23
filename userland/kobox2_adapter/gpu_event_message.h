/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_GPU_EVENT_MESSAGE_H
#define PACHA_KOBOX_GPU_EVENT_MESSAGE_H

#include <kobox2/gpu.h>
#include <string.h>

enum {
    PH_GPU_DRM_EVENT_SET = KB2_GPU_DRM_CORE_SET_ID,
    PH_GPU_DRM_EVENT_READ = KB2_GPU_DRM_CORE_COMMAND_READ_EVENTS,
    PH_GPU_FENCE_EVENT_SET = KB2_GPU_DRM_VIRTGPU_SET_ID,
    PH_GPU_FENCE_EVENT_COMPLETE = KB2_GPU_DRM_VIRTGPU_COMMAND_EXECBUFFER,
    PH_GPU_FENCE_RECORD_BYTES = 24,
    PH_GPU_DRM_EVENT_BYTES = KB2_GPU_DRM_CORE_MAX_EVENT_BYTES,
    PH_GPU_DRM_EVENT_HEADER_BYTES =
        KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + KB2_GPU_NOTIFY_HEADER_SIZE,
    PH_GPU_DRM_EVENT_MESSAGE_BYTES =
        PH_GPU_DRM_EVENT_HEADER_BYTES + PH_GPU_DRM_EVENT_BYTES,
};

struct ph_gpu_drm_event_message {
    uint64_t session_id;
    uint64_t sequence;
    const unsigned char *data;
    size_t data_size;
    int fences;
};

static inline void ph_gpu_event_store_u32(unsigned char *out, uint32_t value) {
    out[0] = (unsigned char)value;
    out[1] = (unsigned char)(value >> 8u);
    out[2] = (unsigned char)(value >> 16u);
    out[3] = (unsigned char)(value >> 24u);
}

static inline void ph_gpu_event_store_u64(unsigned char *out, uint64_t value) {
    ph_gpu_event_store_u32(out, (uint32_t)value);
    ph_gpu_event_store_u32(out + 4, (uint32_t)(value >> 32u));
}

static inline uint32_t ph_gpu_event_load_u32(const unsigned char *in) {
    return (uint32_t)in[0] | (uint32_t)in[1] << 8u |
        (uint32_t)in[2] << 16u | (uint32_t)in[3] << 24u;
}

static inline uint64_t ph_gpu_event_load_u64(const unsigned char *in) {
    return (uint64_t)ph_gpu_event_load_u32(in) |
        (uint64_t)ph_gpu_event_load_u32(in + 4) << 32u;
}

static inline int ph_gpu_event_encode(unsigned char *out, size_t capacity,
    size_t *size_out, uint64_t generation, uint64_t session_id,
    uint64_t sequence, const unsigned char *data, size_t data_size, int fences) {
    if (!out || !size_out || !generation || !sequence ||
        (fences ? session_id || data_size % PH_GPU_FENCE_RECORD_BYTES : !session_id) ||
        data_size > PH_GPU_DRM_EVENT_BYTES || (data_size && !data) ||
        capacity < PH_GPU_DRM_EVENT_HEADER_BYTES + data_size)
        return -1;
    kb2_protocol_message_envelope_t envelope = {
        .protocol_id = KB2_GPU_PROTOCOL_ID,
        .opcode = KB2_GPU_OPCODE_NOTIFY,
        .generation = generation,
        .payload_length = KB2_GPU_NOTIFY_HEADER_SIZE + data_size,
    };
    if (kb2_protocol_message_envelope_encode(out, capacity, &envelope))
        return -1;
    unsigned char *header = out + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE;
    memset(header, 0, KB2_GPU_NOTIFY_HEADER_SIZE);
    ph_gpu_event_store_u64(
        header + KB2_GPU_NOTIFY_HEADER_SESSION_ID_OFFSET, session_id);
    ph_gpu_event_store_u32(
        header + KB2_GPU_NOTIFY_HEADER_EVENT_SET_ID_OFFSET,
        fences ? PH_GPU_FENCE_EVENT_SET : PH_GPU_DRM_EVENT_SET);
    ph_gpu_event_store_u32(
        header + KB2_GPU_NOTIFY_HEADER_EVENT_ID_OFFSET,
        fences ? PH_GPU_FENCE_EVENT_COMPLETE : PH_GPU_DRM_EVENT_READ);
    ph_gpu_event_store_u64(
        header + KB2_GPU_NOTIFY_HEADER_EVENT_SEQUENCE_OFFSET, sequence);
    ph_gpu_event_store_u64(
        header + KB2_GPU_NOTIFY_HEADER_TOPOLOGY_EPOCH_OFFSET, 1);
    ph_gpu_event_store_u32(
        header + KB2_GPU_NOTIFY_HEADER_PAYLOAD_LENGTH_OFFSET,
        (uint32_t)data_size);
    ph_gpu_event_store_u32(
        header + KB2_GPU_NOTIFY_HEADER_PAYLOAD_OFFSET_OFFSET,
        KB2_GPU_NOTIFY_HEADER_SIZE);
    if (data_size)
        memcpy(header + KB2_GPU_NOTIFY_HEADER_SIZE, data, data_size);
    *size_out = PH_GPU_DRM_EVENT_HEADER_BYTES + data_size;
    return 0;
}

static inline int ph_gpu_drm_event_encode(unsigned char *out, size_t capacity,
    size_t *size_out, uint64_t generation, uint64_t session_id,
    uint64_t sequence, const unsigned char *data, size_t data_size) {
    return ph_gpu_event_encode(out, capacity, size_out, generation, session_id,
        sequence, data, data_size, 0);
}

static inline int ph_gpu_drm_event_decode(const unsigned char *in, size_t size,
    uint64_t generation, struct ph_gpu_drm_event_message *out) {
    if (!in || !out || !generation ||
        size < PH_GPU_DRM_EVENT_HEADER_BYTES)
        return -1;
    kb2_protocol_message_envelope_t envelope;
    if (kb2_protocol_message_envelope_decode(in, size, &envelope) ||
        envelope.protocol_id != KB2_GPU_PROTOCOL_ID ||
        envelope.opcode != KB2_GPU_OPCODE_NOTIFY || envelope.flags ||
        envelope.generation != generation || envelope.correlation_id ||
        envelope.payload_length != size - KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE)
        return -1;
    const unsigned char *header = in + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE;
    const uint32_t payload_size = ph_gpu_event_load_u32(
        header + KB2_GPU_NOTIFY_HEADER_PAYLOAD_LENGTH_OFFSET);
    const uint64_t session_id = ph_gpu_event_load_u64(
        header + KB2_GPU_NOTIFY_HEADER_SESSION_ID_OFFSET);
    const uint64_t sequence = ph_gpu_event_load_u64(
        header + KB2_GPU_NOTIFY_HEADER_EVENT_SEQUENCE_OFFSET);
    const uint32_t event_set = ph_gpu_event_load_u32(
        header + KB2_GPU_NOTIFY_HEADER_EVENT_SET_ID_OFFSET);
    const uint32_t event_id = ph_gpu_event_load_u32(
        header + KB2_GPU_NOTIFY_HEADER_EVENT_ID_OFFSET);
    const int fences = event_set == PH_GPU_FENCE_EVENT_SET &&
        event_id == PH_GPU_FENCE_EVENT_COMPLETE;
    /* Fences outlive DRM sessions. Their individual records carry the
     * submitting session/correlation; the batch belongs to the generation. */
    if (!sequence || payload_size > PH_GPU_DRM_EVENT_BYTES ||
        (fences ? session_id || payload_size % PH_GPU_FENCE_RECORD_BYTES :
            !session_id || event_set != PH_GPU_DRM_EVENT_SET ||
                event_id != PH_GPU_DRM_EVENT_READ) ||
        size != PH_GPU_DRM_EVENT_HEADER_BYTES + payload_size ||
        ph_gpu_event_load_u64(
            header + KB2_GPU_NOTIFY_HEADER_TOPOLOGY_EPOCH_OFFSET) != 1 ||
        ph_gpu_event_load_u32(
            header + KB2_GPU_NOTIFY_HEADER_SPAN_COUNT_OFFSET) ||
        ph_gpu_event_load_u32(
            header + KB2_GPU_NOTIFY_HEADER_ATTACHMENT_COUNT_OFFSET) ||
        ph_gpu_event_load_u32(
            header + KB2_GPU_NOTIFY_HEADER_PAYLOAD_OFFSET_OFFSET) !=
                KB2_GPU_NOTIFY_HEADER_SIZE ||
        ph_gpu_event_load_u32(
            header + KB2_GPU_NOTIFY_HEADER_SPAN_TABLE_OFFSET_OFFSET) ||
        ph_gpu_event_load_u32(
            header + KB2_GPU_NOTIFY_HEADER_ATTACHMENT_TABLE_OFFSET_OFFSET))
        return -1;
    for (size_t i = 0; i < 8; ++i)
        if (header[KB2_GPU_NOTIFY_HEADER_RESERVED_OFFSET + i])
            return -1;
    *out = (struct ph_gpu_drm_event_message) {
        .session_id = session_id,
        .sequence = sequence,
        .data = header + KB2_GPU_NOTIFY_HEADER_SIZE,
        .data_size = payload_size,
        .fences = fences,
    };
    return 0;
}

#endif
