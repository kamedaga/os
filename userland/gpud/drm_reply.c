/* SPDX-License-Identifier: MIT */
#include "drm_reply.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

static uint32_t read_u32(const unsigned char *bytes) {
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
        (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static uint64_t read_u64(const unsigned char *bytes) {
    return read_u32(bytes) | (uint64_t)read_u32(bytes + 4) << 32;
}

int gpud_drm_status_errno(uint32_t status) {
    switch (status) {
    case KB2_GPU_STATUS_OK: return 0;
    case KB2_GPU_STATUS_INVALID: return -EINVAL;
    case KB2_GPU_STATUS_UNSUPPORTED: return -EOPNOTSUPP;
    case KB2_GPU_STATUS_DENIED: return -EACCES;
    case KB2_GPU_STATUS_NOT_FOUND: return -ENOENT;
    case KB2_GPU_STATUS_STALE_GENERATION: return -ESTALE;
    case KB2_GPU_STATUS_LIMIT: return -ENOSPC;
    case KB2_GPU_STATUS_NO_MEMORY: return -ENOMEM;
    case KB2_GPU_STATUS_BUSY: return -EBUSY;
    case KB2_GPU_STATUS_TIMED_OUT: return -ETIMEDOUT;
    case KB2_GPU_STATUS_CANCELED: return -ECANCELED;
    case KB2_GPU_STATUS_SESSION_LOST:
    case KB2_GPU_STATUS_DEVICE_LOST: return -EIO;
    default: return -EPROTO;
    }
}

int gpud_drm_ioctl_reply(gpud_drm_ioctl_request_t *request,
    struct gpud_drm_translation *translation, uint64_t correlation,
    const unsigned char *reply, size_t reply_size,
    const unsigned char *output, size_t output_size) {
    if (!request || !translation || !correlation || !reply) return -EINVAL;
    /* Bind output conversion to the exact request admitted by the owner. */
    struct gpud_drm_translation expected;
    struct gpud_drm_binding binding = {.generation = translation->generation,
        .frontend_handle = translation->frontend_handle, .session_id = translation->session_id};
    if (gpud_drm_ioctl_encode(&expected, &binding, request, translation->region.region_id) ||
        expected.command_size != translation->command_size ||
        expected.command_set_id != translation->command_set_id ||
        expected.command_id != translation->command_id ||
        expected.staged_input_size != translation->staged_input_size ||
        memcmp(expected.command, translation->command, expected.command_size) ||
        memcmp(expected.staged_input, translation->staged_input,
            expected.staged_input_size)) return -EPROTO;
    kb2_protocol_message_envelope_t envelope;
    if (kb2_protocol_message_envelope_decode(reply, reply_size, &envelope) ||
        envelope.protocol_id != KB2_GPU_PROTOCOL_ID || envelope.opcode != KB2_GPU_OPCODE_COMMAND ||
        envelope.flags || envelope.generation != binding.generation || envelope.correlation_id != correlation ||
        envelope.payload_length != reply_size - KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE) return -EPROTO;
    kb2_gpu_inline_completion_t completion = {0};
    const int mode_map =
        expected.command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
        expected.command_id == KB2_GPU_DRM_MODE_COMMAND_MAP_DUMB;
    const int virtgpu_map =
        expected.command_set_id == KB2_GPU_DRM_VIRTGPU_SET_ID &&
        expected.command_id == KB2_GPU_DRM_VIRTGPU_COMMAND_MAP;
    if (mode_map || virtgpu_map) {
        kb2_gpu_virtgpu_map_completion_t mapped;
        const kb2_protocol_status_t decoded = mode_map ?
            kb2_gpu_mode_map_completion_decode(
                reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
                envelope.payload_length, binding.session_id,
                binding.generation, &mapped) :
            kb2_gpu_virtgpu_map_completion_decode(
                reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
                envelope.payload_length, binding.session_id,
                binding.generation, &mapped);
        if (decoded)
            return -EPROTO;
        int result = gpud_drm_status_errno(mapped.status);
        if (result) return result;
        if (mapped.rights !=
                (KB2_GPU_SPAN_RIGHT_READ | KB2_GPU_SPAN_RIGHT_WRITE) ||
            mapped.cache_policy || mapped.length > UINT32_MAX)
            return -EPROTO;
        translation->mapping_id = mapped.mapping_id;
        translation->mapping_length = mapped.length;
        translation->mapping_exchange = mapped.exchange_id;
        translation->mapping_rights = (uint32_t)mapped.rights;
        translation->mapping_cache_policy = mapped.cache_policy;
        memcpy(request->data + (mode_map ?
            offsetof(gpud_drm_mode_map_dumb_t, offset) : 0),
            &mapped.mapping_id, sizeof(mapped.mapping_id));
        return 0;
    }
    if (kb2_gpu_inline_completion_decode(reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
        envelope.payload_length, binding.session_id, &completion)) return -EPROTO;
    int result = gpud_drm_status_errno(completion.status);
    if (result) return result;
    if (expected.command_set_id == KB2_GPU_DRM_CORE_SET_ID) switch (expected.command_id) {
    case KB2_GPU_DRM_CORE_COMMAND_VERSION: {
        if (completion.record_schema_id != KB2_GPU_DRM_CORE_RECORD_VERSION_RESULT ||
            completion.length != KB2_GPU_DRM_CORE_RECORD_VERSION_RESULT_SIZE ||
            read_u32(completion.data + 12) || read_u32(completion.data + 28) ||
            read_u32(completion.data) > INT32_MAX || read_u32(completion.data + 4) > INT32_MAX ||
            read_u32(completion.data + 8) > INT32_MAX) return -EPROTO;
        if (expected.region.region_id && (!output || output_size < GPUD_DRM_VERSION_BYTES))
            return -EMSGSIZE;
        gpud_drm_version_wire_t version;
        memcpy(&version, request->data, sizeof(version));
        version.major = (int32_t)read_u32(completion.data);
        version.minor = (int32_t)read_u32(completion.data + 4);
        version.patchlevel = (int32_t)read_u32(completion.data + 8);
        version.name_length = read_u32(completion.data + 16);
        version.date_length = read_u32(completion.data + 20);
        version.desc_length = read_u32(completion.data + 24);
        char *strings[] = {version.name, version.date, version.desc};
        const uint64_t lengths[] = {version.name_length, version.date_length, version.desc_length};
        const size_t offsets[] = {0, GPUD_DRM_VERSION_NAME_BYTES, GPUD_DRM_VERSION_NAME_BYTES + GPUD_DRM_VERSION_DATE_BYTES};
        for (size_t index = 0; index < 3; ++index) {
            size_t copied = expected.version_capacity[index];
            if (lengths[index] < copied) copied = (size_t)lengths[index];
            if (copied) memcpy(strings[index], output + offsets[index], copied);
        }
        memcpy(request->data, &version, sizeof(version));
        return 0;
    }
    case KB2_GPU_DRM_CORE_COMMAND_GET_CAP:
        if (completion.record_schema_id != KB2_GPU_DRM_CORE_RECORD_SCALAR_U64 ||
            completion.length != KB2_GPU_DRM_CORE_RECORD_SCALAR_U64_SIZE) return -EPROTO;
        memcpy(request->data + 8, completion.data, 8);
        return 0;
    case KB2_GPU_DRM_CORE_COMMAND_SET_CLIENT_CAP:
    case KB2_GPU_DRM_CORE_COMMAND_GEM_CLOSE:
    case KB2_GPU_DRM_CORE_COMMAND_SYNCOBJ_DESTROY:
    case KB2_GPU_DRM_CORE_COMMAND_SYNCOBJ_RESET:
    case KB2_GPU_DRM_CORE_COMMAND_SYNCOBJ_SIGNAL:
        return completion.length || completion.record_schema_id ? -EPROTO : 0;
    case KB2_GPU_DRM_CORE_COMMAND_SYNCOBJ_CREATE:
        if (completion.record_schema_id !=
                KB2_GPU_DRM_CORE_RECORD_SYNCOBJ_HANDLE_RESULT ||
            completion.length != KB2_GPU_DRM_CORE_RECORD_SYNCOBJ_HANDLE_RESULT_SIZE ||
            !read_u32(completion.data) || read_u32(completion.data + 4))
            return -EPROTO;
        memcpy(request->data, completion.data, sizeof(uint32_t));
        return 0;
    case KB2_GPU_DRM_CORE_COMMAND_SYNCOBJ_WAIT:
        if (completion.record_schema_id != KB2_GPU_DRM_CORE_RECORD_WAIT_RESULT ||
            completion.length != KB2_GPU_DRM_CORE_RECORD_WAIT_RESULT_SIZE ||
            read_u32(completion.data + 4))
            return -EPROTO;
        memcpy(request->data + 24, completion.data, sizeof(uint32_t));
        return 0;
    default:
        return -EOPNOTSUPP;
    }
    if (expected.command_set_id == KB2_GPU_DRM_MODE_SET_ID) {
        switch (expected.command_id) {
        case KB2_GPU_DRM_MODE_COMMAND_GET_MAGIC:
            if (completion.record_schema_id != KB2_GPU_DRM_MODE_RECORD_MAGIC ||
                completion.length != KB2_GPU_DRM_MODE_RECORD_MAGIC_SIZE)
                return -EPROTO;
            memcpy(request->data, completion.data, sizeof(uint32_t));
            return 0;
        case KB2_GPU_DRM_MODE_COMMAND_AUTH_MAGIC:
        case KB2_GPU_DRM_MODE_COMMAND_SET_MASTER:
        case KB2_GPU_DRM_MODE_COMMAND_DROP_MASTER:
        case KB2_GPU_DRM_MODE_COMMAND_SET_CRTC:
        case KB2_GPU_DRM_MODE_COMMAND_CURSOR:
        case KB2_GPU_DRM_MODE_COMMAND_CURSOR2:
        case KB2_GPU_DRM_MODE_COMMAND_PAGE_FLIP:
        case KB2_GPU_DRM_MODE_COMMAND_DIRTY_FB:
        case KB2_GPU_DRM_MODE_COMMAND_REMOVE_FB:
            return completion.length || completion.record_schema_id ?
                -EPROTO : 0;
        case KB2_GPU_DRM_MODE_COMMAND_CREATE_DUMB:
            if (completion.record_schema_id !=
                    KB2_GPU_DRM_MODE_RECORD_DUMB_CREATE_RESULT ||
                completion.length !=
                    KB2_GPU_DRM_MODE_RECORD_DUMB_CREATE_RESULT_SIZE ||
                !read_u32(completion.data +
                    KB2_GPU_DRM_MODE_RECORD_DUMB_CREATE_RESULT_HANDLE_OFFSET) ||
                !read_u32(completion.data +
                    KB2_GPU_DRM_MODE_RECORD_DUMB_CREATE_RESULT_PITCH_OFFSET) ||
                !read_u64(completion.data +
                    KB2_GPU_DRM_MODE_RECORD_DUMB_CREATE_RESULT_SIZE_OFFSET))
                return -EPROTO;
            memcpy(request->data +
                    offsetof(gpud_drm_mode_create_dumb_t, handle),
                completion.data,
                KB2_GPU_DRM_MODE_RECORD_DUMB_CREATE_RESULT_SIZE);
            return 0;
        case KB2_GPU_DRM_MODE_COMMAND_GET_RESOURCES: {
            if (completion.record_schema_id !=
                    KB2_GPU_DRM_MODE_RECORD_RESOURCES_RESULT ||
                completion.length !=
                    KB2_GPU_DRM_MODE_RECORD_RESOURCES_RESULT_SIZE ||
                read_u32(completion.data) != 1 ||
                read_u32(completion.data + 4) ||
                (!output && expected.region.length) ||
                output_size < expected.region.length)
                return -EPROTO;
            gpud_drm_kms_resources_wire_t wire;
            memcpy(&wire, request->data, sizeof(wire));
            uint32_t *arrays[4] = {wire.fbs, wire.crtcs,
                wire.connectors, wire.encoders};
            for (size_t i = 0; i < 4; ++i)
                if (expected.output_capacity[i])
                    memcpy(arrays[i], output + expected.output_offset[i],
                        expected.output_capacity[i] * sizeof(uint32_t));
            wire.value.count_fbs = read_u32(completion.data + 8);
            wire.value.count_crtcs = read_u32(completion.data + 12);
            wire.value.count_connectors = read_u32(completion.data + 16);
            wire.value.count_encoders = read_u32(completion.data + 20);
            wire.value.min_width = read_u32(completion.data + 24);
            wire.value.max_width = read_u32(completion.data + 28);
            wire.value.min_height = read_u32(completion.data + 32);
            wire.value.max_height = read_u32(completion.data + 36);
            memcpy(request->data, &wire, sizeof(wire));
            return 0;
        }
        case KB2_GPU_DRM_MODE_COMMAND_GET_CONNECTOR: {
            gpud_drm_kms_connector_wire_t wire;
            memcpy(&wire, request->data, sizeof(wire));
            if (completion.record_schema_id !=
                    KB2_GPU_DRM_MODE_RECORD_CONNECTOR_RESULT ||
                completion.length !=
                    KB2_GPU_DRM_MODE_RECORD_CONNECTOR_RESULT_SIZE ||
                read_u32(completion.data) != 1 ||
                read_u32(completion.data + 4) ||
                read_u32(completion.data + 12) != wire.value.connector_id ||
                read_u32(completion.data + 52) ||
                (!output && expected.region.length) ||
                output_size < expected.region.length)
                return -EPROTO;
            if (expected.output_capacity[0])
                memcpy(wire.modes, output + expected.output_offset[0],
                    expected.output_capacity[0] * sizeof(wire.modes[0]));
            for (size_t i = 0; i < expected.output_capacity[1]; ++i) {
                const unsigned char *property = output +
                    expected.output_offset[1] +
                    i * KB2_GPU_DRM_MODE_RECORD_PROPERTY_VALUE_SIZE;
                if (read_u32(property + 4))
                    return -EPROTO;
                wire.props[i] = read_u32(property);
                wire.prop_values[i] = read_u64(property + 8);
            }
            if (expected.output_capacity[2])
                memcpy(wire.encoders, output + expected.output_offset[2],
                    expected.output_capacity[2] * sizeof(wire.encoders[0]));
            wire.value.encoder_id = read_u32(completion.data + 8);
            wire.value.connector_type = read_u32(completion.data + 16);
            wire.value.connector_type_id = read_u32(completion.data + 20);
            wire.value.connection = read_u32(completion.data + 24);
            wire.value.mm_width = read_u32(completion.data + 28);
            wire.value.mm_height = read_u32(completion.data + 32);
            wire.value.subpixel = read_u32(completion.data + 36);
            wire.value.count_modes = read_u32(completion.data + 40);
            wire.value.count_props = read_u32(completion.data + 44);
            wire.value.count_encoders = read_u32(completion.data + 48);
            wire.value.pad = 0;
            memcpy(request->data, &wire, sizeof(wire));
            return 0;
        }
        case KB2_GPU_DRM_MODE_COMMAND_GET_ENCODER: {
            gpud_drm_mode_get_encoder_t encoder;
            memcpy(&encoder, request->data, sizeof(encoder));
            if (completion.record_schema_id !=
                    KB2_GPU_DRM_MODE_RECORD_ENCODER_RESULT ||
                completion.length !=
                    KB2_GPU_DRM_MODE_RECORD_ENCODER_RESULT_SIZE ||
                read_u32(completion.data) != 1 ||
                read_u32(completion.data + 4) ||
                read_u32(completion.data + 8) != encoder.encoder_id ||
                read_u32(completion.data + 28))
                return -EPROTO;
            memcpy(request->data, completion.data + 8, sizeof(encoder));
            return 0;
        }
        case KB2_GPU_DRM_MODE_COMMAND_GET_CRTC: {
            gpud_drm_kms_crtc_wire_t wire;
            memcpy(&wire, request->data, sizeof(wire));
            if (completion.record_schema_id !=
                    KB2_GPU_DRM_MODE_RECORD_CRTC_RESULT ||
                completion.length != KB2_GPU_DRM_MODE_RECORD_CRTC_RESULT_SIZE ||
                read_u64(completion.data) != 1 ||
                read_u32(completion.data + 8) != wire.value.crtc_id ||
                read_u32(completion.data + 28) > 1 ||
                read_u64(completion.data + 32) || !output ||
                output_size < KB2_GPU_DRM_MODE_RECORD_MODE_INFO_SIZE)
                return -EPROTO;
            wire.value.fb_id = read_u32(completion.data + 12);
            wire.value.x = read_u32(completion.data + 16);
            wire.value.y = read_u32(completion.data + 20);
            wire.value.gamma_size = read_u32(completion.data + 24);
            wire.value.mode_valid = read_u32(completion.data + 28);
            if (wire.value.mode_valid)
                memcpy(&wire.value.mode, output, sizeof(wire.value.mode));
            else
                memset(&wire.value.mode, 0, sizeof(wire.value.mode));
            memcpy(request->data, &wire, sizeof(wire));
            return 0;
        }
        case KB2_GPU_DRM_MODE_COMMAND_ADD_FB:
        case KB2_GPU_DRM_MODE_COMMAND_ADD_FB2:
            if (completion.record_schema_id !=
                    KB2_GPU_DRM_MODE_RECORD_OBJECT_ID_RESULT ||
                completion.length !=
                    KB2_GPU_DRM_MODE_RECORD_OBJECT_ID_RESULT_SIZE ||
                read_u64(completion.data) != 1 ||
                !read_u32(completion.data + 8) ||
                read_u32(completion.data + 12))
                return -EPROTO;
            memcpy(request->data, completion.data + 8, sizeof(uint32_t));
            return 0;
        case KB2_GPU_DRM_MODE_COMMAND_OBJECT_GET_PROPERTIES: {
            gpud_drm_kms_object_properties_wire_t wire;
            if (completion.record_schema_id !=
                    KB2_GPU_DRM_MODE_RECORD_TOPOLOGY_COUNT_RESULT ||
                completion.length !=
                    KB2_GPU_DRM_MODE_RECORD_TOPOLOGY_COUNT_RESULT_SIZE ||
                read_u64(completion.data) != 1 ||
                (!output && expected.region.length) ||
                output_size < expected.region.length)
                return -EPROTO;
            const uint32_t count = read_u32(completion.data +
                KB2_GPU_DRM_MODE_RECORD_TOPOLOGY_COUNT_RESULT_COUNT_OFFSET);
            const uint32_t required = read_u32(completion.data +
                KB2_GPU_DRM_MODE_RECORD_TOPOLOGY_COUNT_RESULT_REQUIRED_COUNT_OFFSET);
            const uint32_t capacity = expected.output_capacity[0];
            if (count != (required < capacity ? required : capacity))
                return -EPROTO;
            memcpy(&wire, request->data, sizeof(wire));
            for (size_t i = 0; i < count; ++i) {
                const unsigned char *property = output +
                    i * KB2_GPU_DRM_MODE_RECORD_PROPERTY_VALUE_SIZE;
                if (read_u32(property +
                        KB2_GPU_DRM_MODE_RECORD_PROPERTY_VALUE_RESERVED_OFFSET))
                    return -EPROTO;
                wire.props[i] = read_u32(property +
                    KB2_GPU_DRM_MODE_RECORD_PROPERTY_VALUE_PROPERTY_ID_OFFSET);
                wire.prop_values[i] = read_u64(property +
                    KB2_GPU_DRM_MODE_RECORD_PROPERTY_VALUE_VALUE_OFFSET);
            }
            wire.value.count_props = required;
            memcpy(request->data, &wire, sizeof(wire));
            return 0;
        }
        default:
            return -EOPNOTSUPP;
        }
    }
    if (expected.command_set_id != KB2_GPU_DRM_VIRTGPU_SET_ID)
        return -EOPNOTSUPP;
    switch (expected.command_id) {
    case KB2_GPU_DRM_VIRTGPU_COMMAND_GETPARAM:
        if (completion.record_schema_id != KB2_GPU_DRM_VIRTGPU_RECORD_SCALAR_U64 ||
            completion.length != KB2_GPU_DRM_VIRTGPU_RECORD_SCALAR_U64_SIZE)
            return -EPROTO;
        memcpy(request->data + 8, completion.data, 8);
        return 0;
    case KB2_GPU_DRM_VIRTGPU_COMMAND_EXECBUFFER:
        if (completion.record_schema_id != KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_RESULT ||
            completion.length != KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_RESULT_SIZE ||
            read_u32(completion.data +
                KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_RESULT_FLAGS_OFFSET) ||
            read_u32(completion.data +
                KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_RESULT_RESERVED_OFFSET))
            return -EPROTO;
        return 0;
    case KB2_GPU_DRM_VIRTGPU_COMMAND_RESOURCE_CREATE:
        if (completion.record_schema_id !=
                KB2_GPU_DRM_VIRTGPU_RECORD_RESOURCE_CREATE_RESULT ||
            completion.length !=
                KB2_GPU_DRM_VIRTGPU_RECORD_RESOURCE_CREATE_RESULT_SIZE)
            return -EPROTO;
        memcpy(request->data + 40, completion.data, 8);
        memcpy(request->data + 48, completion.data + 8, 8);
        return 0;
    case KB2_GPU_DRM_VIRTGPU_COMMAND_RESOURCE_INFO:
        if (completion.record_schema_id !=
                KB2_GPU_DRM_VIRTGPU_RECORD_RESOURCE_INFO_RESULT ||
            completion.length !=
                KB2_GPU_DRM_VIRTGPU_RECORD_RESOURCE_INFO_RESULT_SIZE ||
            read_u32(completion.data + 12))
            return -EPROTO;
        memcpy(request->data + 4, completion.data, 12);
        return 0;
    case KB2_GPU_DRM_VIRTGPU_COMMAND_TRANSFER_FROM_HOST_3D:
    case KB2_GPU_DRM_VIRTGPU_COMMAND_TRANSFER_TO_HOST_3D:
    case KB2_GPU_DRM_VIRTGPU_COMMAND_WAIT:
        return completion.length || completion.record_schema_id ? -EPROTO : 0;
    case KB2_GPU_DRM_VIRTGPU_COMMAND_GET_CAPS: {
        if (completion.record_schema_id != KB2_GPU_DRM_VIRTGPU_RECORD_GET_CAPS_RESULT ||
            completion.length != KB2_GPU_DRM_VIRTGPU_RECORD_GET_CAPS_RESULT_SIZE)
            return -EPROTO;
        uint32_t response = read_u32(completion.data);
        uint32_t required = read_u32(completion.data + 4);
        /* These are the deterministic transport extent, not a guessed host
         * capset length (the public virtio-gpu ioctl does not return one). */
        return response != request->aux_size || required != request->aux_size ?
            -EPROTO : 0;
    }
    case KB2_GPU_DRM_VIRTGPU_COMMAND_CONTEXT_INIT:
        return completion.length || completion.record_schema_id ? -EPROTO : 0;
    default:
        return -EOPNOTSUPP;
    }
}

static int event_completion(const struct gpud_drm_translation *translation,
    const struct gpud_drm_translation *expected, uint64_t correlation,
    const unsigned char *reply, size_t reply_size,
    kb2_gpu_inline_completion_t *completion) {
    if (!translation || !expected || !correlation || !reply || !completion ||
        expected->command_size != translation->command_size ||
        expected->command_set_id != translation->command_set_id ||
        expected->command_id != translation->command_id ||
        memcmp(expected->command, translation->command, expected->command_size))
        return -EPROTO;
    kb2_protocol_message_envelope_t envelope;
    if (kb2_protocol_message_envelope_decode(reply, reply_size, &envelope) ||
        envelope.protocol_id != KB2_GPU_PROTOCOL_ID ||
        envelope.opcode != KB2_GPU_OPCODE_COMMAND || envelope.flags ||
        envelope.generation != translation->generation ||
        envelope.correlation_id != correlation ||
        envelope.payload_length != reply_size - KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE ||
        kb2_gpu_inline_completion_decode(
            reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
            envelope.payload_length, translation->session_id, completion))
        return -EPROTO;
    return gpud_drm_status_errno(completion->status);
}

static int prime_envelope(const struct gpud_drm_translation *translation,
    const struct gpud_drm_translation *expected, uint64_t correlation,
    const unsigned char *reply, size_t reply_size,
    kb2_protocol_message_envelope_t *envelope) {
    if (!translation || !expected || !correlation || !reply || !envelope ||
        expected->command_size != translation->command_size ||
        expected->command_set_id != translation->command_set_id ||
        expected->command_id != translation->command_id ||
        memcmp(expected->command, translation->command, expected->command_size) ||
        kb2_protocol_message_envelope_decode(reply, reply_size, envelope) ||
        envelope->protocol_id != KB2_GPU_PROTOCOL_ID ||
        envelope->opcode != KB2_GPU_OPCODE_COMMAND || envelope->flags ||
        envelope->generation != translation->generation ||
        envelope->correlation_id != correlation ||
        envelope->payload_length !=
            reply_size - KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE)
        return -EPROTO;
    return 0;
}

int gpud_drm_prime_export_reply(const struct gpud_drm_translation *translation,
    uint32_t handle, uint32_t flags, uint64_t correlation,
    const unsigned char *reply, size_t reply_size, uint64_t *token) {
    if (!translation || !token)
        return -EINVAL;
    const struct gpud_drm_binding binding = {
        .generation = translation->generation,
        .frontend_handle = translation->frontend_handle,
        .session_id = translation->session_id,
    };
    struct gpud_drm_translation expected;
    kb2_protocol_message_envelope_t envelope;
    kb2_gpu_attachment_completion_t completion;
    int result = gpud_drm_prime_export_encode(
        &expected, &binding, handle, flags);
    if (result)
        return result;
    result = prime_envelope(translation, &expected, correlation,
        reply, reply_size, &envelope);
    if (result || kb2_gpu_attachment_completion_decode(
            reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
            envelope.payload_length, binding.session_id,
            binding.generation, &completion))
        return result ? result : -EPROTO;
    result = gpud_drm_status_errno(completion.status);
    if (result)
        return result;
    if (completion.argument_id !=
            KB2_GPU_DRM_CORE_COMMAND_PRIME_HANDLE_TO_ATTACHMENT_COMPLETION_ATTACHMENT_DMA_BUFFER_ARGUMENT_ID ||
        completion.attachment.object_class != KB2_GPU_ATTACHMENT_DMA_BUF ||
        completion.attachment.rights !=
            (KB2_GPU_SPAN_RIGHT_READ | KB2_GPU_SPAN_RIGHT_WRITE) ||
        completion.attachment.role !=
            KB2_GPU_DRM_CORE_COMMAND_PRIME_HANDLE_TO_ATTACHMENT_COMPLETION_ATTACHMENT_DMA_BUFFER_ROLE ||
        completion.attachment.ownership != KB2_GPU_ATTACHMENT_MOVE ||
        completion.attachment.flags != KB2_GPU_ATTACHMENT_FLAG_OUTPUT)
        return -EPROTO;
    *token = completion.attachment.exchange_id;
    return 0;
}

int gpud_drm_prime_import_reply(const struct gpud_drm_translation *translation,
    uint64_t token, uint64_t correlation,
    const unsigned char *reply, size_t reply_size, uint32_t *handle) {
    if (!translation || !handle)
        return -EINVAL;
    const struct gpud_drm_binding binding = {
        .generation = translation->generation,
        .frontend_handle = translation->frontend_handle,
        .session_id = translation->session_id,
    };
    struct gpud_drm_translation expected;
    kb2_protocol_message_envelope_t envelope;
    kb2_gpu_inline_completion_t completion;
    int result = gpud_drm_prime_import_encode(&expected, &binding, token);
    if (result)
        return result;
    result = prime_envelope(translation, &expected, correlation,
        reply, reply_size, &envelope);
    if (result || kb2_gpu_inline_completion_decode(
            reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
            envelope.payload_length, binding.session_id, &completion))
        return result ? result : -EPROTO;
    result = gpud_drm_status_errno(completion.status);
    if (result)
        return result;
    if (completion.record_schema_id !=
            KB2_GPU_DRM_CORE_RECORD_SYNCOBJ_HANDLE_RESULT ||
        completion.length != KB2_GPU_DRM_CORE_RECORD_SYNCOBJ_HANDLE_RESULT_SIZE ||
        !read_u32(completion.data) || read_u32(completion.data + 4))
        return -EPROTO;
    *handle = read_u32(completion.data);
    return 0;
}

int gpud_drm_poll_reply(uint32_t requested, uint32_t *ready,
    const struct gpud_drm_translation *translation, uint64_t correlation,
    const unsigned char *reply, size_t reply_size) {
    if (!ready || !translation)
        return -EINVAL;
    struct gpud_drm_binding binding = {.generation = translation->generation,
        .frontend_handle = translation->frontend_handle,
        .session_id = translation->session_id};
    struct gpud_drm_translation expected;
    kb2_gpu_inline_completion_t completion = {0};
    int result = gpud_drm_poll_encode(&expected, &binding, requested);
    if (result)
        return result;
    result = event_completion(translation, &expected, correlation,
        reply, reply_size, &completion);
    if (result)
        return result;
    if (completion.record_schema_id != KB2_GPU_DRM_CORE_RECORD_SCALAR_U32 ||
        completion.length != KB2_GPU_DRM_CORE_RECORD_SCALAR_U32_SIZE)
        return -EPROTO;
    uint32_t value = read_u32(completion.data);
    if (value & ~requested)
        return -EPROTO;
    *ready = value;
    return 0;
}

int gpud_drm_read_reply(gpud_drm_read_request_t *request,
    const struct gpud_drm_translation *translation, uint64_t correlation,
    const unsigned char *reply, size_t reply_size,
    const unsigned char *output, size_t output_size) {
    if (!request || !translation || request->capacity > UINT32_MAX)
        return -EINVAL;
    struct gpud_drm_binding binding = {.generation = translation->generation,
        .frontend_handle = translation->frontend_handle,
        .session_id = translation->session_id};
    struct gpud_drm_translation expected;
    kb2_gpu_inline_completion_t completion = {0};
    int result = gpud_drm_read_encode(&expected, &binding,
        (uint32_t)request->capacity, translation->region.region_id);
    if (result)
        return result;
    result = event_completion(translation, &expected, correlation,
        reply, reply_size, &completion);
    if (result)
        return result;
    if (completion.record_schema_id != KB2_GPU_DRM_CORE_RECORD_LENGTH_RESULT ||
        completion.length != KB2_GPU_DRM_CORE_RECORD_LENGTH_RESULT_SIZE)
        return -EPROTO;
    uint32_t bytes = read_u32(completion.data);
    uint32_t required = read_u32(completion.data + 4);
    if (bytes != required || bytes > request->capacity ||
        bytes > output_size || (bytes && !output))
        return -EPROTO;
    if (bytes)
        memcpy(request->data, output, bytes);
    request->data_size = bytes;
    return 0;
}
