/* SPDX-License-Identifier: MIT */
#include "drm_reply.h"
#include <gpud/drm_kms_query.h>

#include <kobox2/amdgpu_info_shape.h>

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

static int mode_query_aux_reply(gpud_drm_ioctl_request_t *request,
    const struct gpud_drm_translation *expected,
    const kb2_gpu_inline_completion_t *completion)
{
    if (request->aux_size != expected->region.length || completion->length < 8 ||
        read_u64(completion->data) != 1) return -EPROTO;
    const unsigned char *data = completion->data;
    switch (request->request) {
    case GPUD_DRM_IOCTL_MODE_GETRESOURCES: {
        if (completion->record_schema_id != KB2_GPU_DRM_MODE_RECORD_RESOURCES_RESULT ||
            completion->length != KB2_GPU_DRM_MODE_RECORD_RESOURCES_RESULT_SIZE)
            return -EPROTO;
        gpud_drm_mode_card_res_t value;
        memcpy(&value, request->data, sizeof(value));
        memcpy(&value.count_fbs, data + 8, 32);
        memcpy(request->data, &value, sizeof(value));
        return 0;
    }
    case GPUD_DRM_IOCTL_MODE_GETPLANERESOURCES:
    case GPUD_DRM_IOCTL_MODE_OBJ_GETPROPERTIES: {
        if (completion->record_schema_id != KB2_GPU_DRM_MODE_RECORD_TOPOLOGY_COUNT_RESULT ||
            completion->length != KB2_GPU_DRM_MODE_RECORD_TOPOLOGY_COUNT_RESULT_SIZE)
            return -EPROTO;
        const uint32_t required = read_u32(data + 12);
        const uint32_t capacity = expected->output_capacity[0];
        if (read_u32(data + 8) != (required < capacity ? required : capacity))
            return -EPROTO;
        const size_t count_offset = request->request == GPUD_DRM_IOCTL_MODE_GETPLANERESOURCES ?
            offsetof(gpud_drm_mode_get_plane_res_t, count_planes) :
            offsetof(gpud_drm_mode_obj_get_properties_t, count_props);
        memcpy(request->data + count_offset, data + 12, 4);
        return 0;
    }
    case GPUD_DRM_IOCTL_MODE_GETPLANE: {
        if (completion->record_schema_id != KB2_GPU_DRM_MODE_RECORD_PLANE_RESULT ||
            completion->length != KB2_GPU_DRM_MODE_RECORD_PLANE_RESULT_SIZE ||
            read_u32(data + 8) != read_u32(request->data) || read_u32(data + 32))
            return -EPROTO;
        memcpy(request->data + 4, data + 12, 20);
        return 0;
    }
    case GPUD_DRM_IOCTL_MODE_GETPROPERTY: {
        if (completion->record_schema_id != KB2_GPU_DRM_MODE_RECORD_PROPERTY_RESULT ||
            completion->length != KB2_GPU_DRM_MODE_RECORD_PROPERTY_RESULT_SIZE ||
            read_u32(data + 20)) return -EPROTO;
        gpud_drm_mode_get_property_t value;
        memcpy(&value, request->data, sizeof(value));
        value.flags = read_u32(data + 8);
        value.count_values = read_u32(data + 12);
        value.count_enum_blobs = read_u32(data + 16);
        memcpy(value.name, data + 24, sizeof(value.name));
        memcpy(request->data, &value, sizeof(value));
        return 0;
    }
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
        .frontend_handle = translation->frontend_handle, .session_id = translation->session_id,
        .profile_kind = translation->profile_kind};
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
    const int amdgpu_map =
        expected.command_set_id == KB2_GPU_DRM_AMDGPU_SET_ID &&
        expected.command_id == KB2_GPU_DRM_AMDGPU_COMMAND_GEM_MMAP;
    if (mode_map || virtgpu_map || amdgpu_map) {
        kb2_gpu_virtgpu_map_completion_t mapped;
        const kb2_protocol_status_t decoded = amdgpu_map ?
            kb2_gpu_amdgpu_map_completion_decode(
                reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
                envelope.payload_length, binding.session_id,
                binding.generation, &mapped) :
            mode_map ?
            kb2_gpu_mode_map_completion_decode(
                reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
                envelope.payload_length, binding.session_id,
                binding.generation, &mapped) :
            kb2_gpu_virtgpu_map_completion_decode(
                reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
                envelope.payload_length, binding.session_id,
                binding.generation, &mapped);
        if (decoded) {
            /* Failed mappings carry the ordinary typed status envelope, not
             * a memory attachment. Preserve the driver error instead of
             * turning every rejected map into an artificial EPROTO. */
            if (kb2_gpu_inline_completion_decode(
                    reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
                    envelope.payload_length, binding.session_id,
                    &completion) || completion.status == KB2_GPU_STATUS_OK ||
                completion.record_schema_id || completion.length)
                return -EPROTO;
            return gpud_drm_status_errno(completion.status);
        }
        int result = gpud_drm_status_errno(mapped.status);
        if (result) return result;
        if (mapped.rights !=
                (KB2_GPU_SPAN_RIGHT_READ | KB2_GPU_SPAN_RIGHT_WRITE) ||
            mapped.cache_policy > KB2_GPU_MAPPING_CACHE_UC || mapped.length > UINT32_MAX)
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
    /* Linux's DIRTYFB reports ENOSYS when a framebuffer has no dirty hook.
     * The protocol's UNSUPPORTED class intentionally groups ENOSYS/ENOTTY/
     * EOPNOTSUPP. Restore this operation's UAPI meaning here: Xorg disables
     * unnecessary dirty tracking on ENOSYS, but retries EOPNOTSUPP on every
     * damaged frame. Other operations retain their own unsupported mapping. */
    if (completion.status == KB2_GPU_STATUS_UNSUPPORTED &&
        expected.command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
        expected.command_id == KB2_GPU_DRM_MODE_COMMAND_DIRTY_FB)
        return -ENOSYS;
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
    case KB2_GPU_DRM_CORE_COMMAND_GET_CLIENT:
        if (completion.record_schema_id != KB2_GPU_DRM_CORE_RECORD_CLIENT_RESULT ||
            completion.length != KB2_GPU_DRM_CORE_RECORD_CLIENT_RESULT_SIZE ||
            read_u32(completion.data + KB2_GPU_DRM_CORE_RECORD_CLIENT_RESULT_RESERVED_OFFSET) ||
            read_u32(completion.data + KB2_GPU_DRM_CORE_RECORD_CLIENT_RESULT_AUTHENTICATED_OFFSET) > 1)
            return -EPROTO;
        memcpy(request->data + 4,
            completion.data + KB2_GPU_DRM_CORE_RECORD_CLIENT_RESULT_AUTHENTICATED_OFFSET, 4);
        memcpy(request->data + 8,
            completion.data + KB2_GPU_DRM_CORE_RECORD_CLIENT_RESULT_PROCESS_ID_OFFSET, 32);
        return 0;
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
        struct gpud_kms_query_shape query_shape;
        if (gpud_kms_query_shape(request->request, &query_shape) &&
            request->data_size == query_shape.scalar_size)
            return mode_query_aux_reply(request, &expected, &completion);
        switch (expected.command_id) {
        case KB2_GPU_DRM_MODE_COMMAND_GET_MAGIC:
            if (completion.record_schema_id != KB2_GPU_DRM_MODE_RECORD_MAGIC ||
                completion.length != KB2_GPU_DRM_MODE_RECORD_MAGIC_SIZE)
                return -EPROTO;
            memcpy(request->data, completion.data, sizeof(uint32_t));
            return 0;

        case KB2_GPU_DRM_MODE_COMMAND_WAIT_VBLANK: {
            if (completion.record_schema_id !=
                    KB2_GPU_DRM_MODE_RECORD_WAIT_VBLANK_RESULT ||
                completion.length !=
                    KB2_GPU_DRM_MODE_RECORD_WAIT_VBLANK_RESULT_SIZE)
                return -EPROTO;
            const uint64_t timestamp = read_u64(completion.data + 8);
            if (timestamp / UINT64_C(1000000000) > INT64_MAX)
                return -EPROTO;
            memcpy(request->data, completion.data, 8);
            const int64_t seconds = (int64_t)(timestamp / UINT64_C(1000000000));
            const int64_t microseconds =
                (int64_t)((timestamp % UINT64_C(1000000000)) / 1000);
            memcpy(request->data + 8, &seconds, sizeof(seconds));
            memcpy(request->data + 16, &microseconds, sizeof(microseconds));
            return 0;
        }
        case KB2_GPU_DRM_MODE_COMMAND_CRTC_GET_SEQUENCE:
            if (completion.record_schema_id !=
                    KB2_GPU_DRM_MODE_RECORD_CRTC_SEQUENCE_RESULT ||
                completion.length !=
                    KB2_GPU_DRM_MODE_RECORD_CRTC_SEQUENCE_RESULT_SIZE ||
                read_u32(completion.data) > 1 ||
                read_u32(completion.data + 4) ||
                read_u64(completion.data + 16) > INT64_MAX)
                return -EPROTO;
            memcpy(request->data + 4, completion.data, 4);
            memcpy(request->data + 8, completion.data + 8, 16);
            return 0;
        case KB2_GPU_DRM_MODE_COMMAND_CRTC_QUEUE_SEQUENCE:
            if (completion.record_schema_id !=
                    KB2_GPU_DRM_MODE_RECORD_SEQUENCE_RESULT ||
                completion.length !=
                    KB2_GPU_DRM_MODE_RECORD_SEQUENCE_RESULT_SIZE)
                return -EPROTO;
            memcpy(request->data + 8, completion.data, 8);
            return 0;
        case KB2_GPU_DRM_MODE_COMMAND_GET_FB:
            if (completion.record_schema_id !=
                    KB2_GPU_DRM_MODE_RECORD_FB_LEGACY ||
                completion.length != KB2_GPU_DRM_MODE_RECORD_FB_LEGACY_SIZE ||
                read_u64(completion.data) != 1 ||
                read_u32(completion.data + 8) != read_u32(request->data) ||
                read_u32(completion.data + 36))
                return -EPROTO;
            memcpy(request->data, completion.data + 8,
                sizeof(gpud_drm_mode_fb_cmd_t));
            return 0;
        case KB2_GPU_DRM_MODE_COMMAND_GET_FB2:
            if (completion.record_schema_id != KB2_GPU_DRM_MODE_RECORD_FB2 ||
                completion.length != KB2_GPU_DRM_MODE_RECORD_FB2_SIZE ||
                read_u64(completion.data) != 1 ||
                read_u32(completion.data + 8) != read_u32(request->data))
                return -EPROTO;
            memcpy(request->data, completion.data + 8, 20);
            memcpy(request->data + 20, completion.data + 28, 48);
            memset(request->data + 68, 0, 4);
            memcpy(request->data + 72, completion.data + 76, 32);
            return 0;
        case KB2_GPU_DRM_MODE_COMMAND_GET_GAMMA: {
            const uint32_t count = read_u32(request->data + 4);
            return completion.record_schema_id ==
                    KB2_GPU_DRM_MODE_RECORD_COUNT_RESULT &&
                completion.length == KB2_GPU_DRM_MODE_RECORD_COUNT_RESULT_SIZE &&
                read_u32(completion.data) == count &&
                read_u32(completion.data + 4) == count &&
                request->aux_size == (uint64_t)count * 6 ? 0 : -EPROTO;
        }

        case KB2_GPU_DRM_MODE_COMMAND_AUTH_MAGIC:
        case KB2_GPU_DRM_MODE_COMMAND_SET_MASTER:
        case KB2_GPU_DRM_MODE_COMMAND_DROP_MASTER:
        case KB2_GPU_DRM_MODE_COMMAND_SET_CRTC:
        case KB2_GPU_DRM_MODE_COMMAND_SET_PLANE:
        case KB2_GPU_DRM_MODE_COMMAND_SET_GAMMA:
        case KB2_GPU_DRM_MODE_COMMAND_CURSOR:
        case KB2_GPU_DRM_MODE_COMMAND_CURSOR2:
        case KB2_GPU_DRM_MODE_COMMAND_PAGE_FLIP:
        case KB2_GPU_DRM_MODE_COMMAND_DIRTY_FB:
        case KB2_GPU_DRM_MODE_COMMAND_REMOVE_FB:
        case KB2_GPU_DRM_MODE_COMMAND_DESTROY_DUMB:
        case KB2_GPU_DRM_MODE_COMMAND_SET_PROPERTY:
        case KB2_GPU_DRM_MODE_COMMAND_DESTROY_PROPERTY_BLOB:
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
            for (size_t i = 0; i < 4; ++i) {
                const uint32_t count = read_u32(completion.data + 8 + i * 4);
                if (count && count <= expected.output_capacity[i])
                    memcpy(arrays[i], output + expected.output_offset[i],
                        count * sizeof(uint32_t));
            }
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
            const int large = request->data_size == sizeof(wire.value);
            memset(&wire, 0, sizeof(wire));
            memcpy(&wire, request->data, request->data_size);
            if (completion.record_schema_id !=
                    KB2_GPU_DRM_MODE_RECORD_CONNECTOR_RESULT ||
                completion.length !=
                    KB2_GPU_DRM_MODE_RECORD_CONNECTOR_RESULT_SIZE ||
                read_u32(completion.data) != 1 ||
                read_u32(completion.data + 4) ||
                read_u32(completion.data + 12) != wire.value.connector_id ||
                read_u32(completion.data + 52) ||
                (large ? request->aux_size != expected.region.length :
                    ((!output && expected.region.length) ||
                     output_size < expected.region.length)))
                return -EPROTO;
            const uint32_t mode_count = read_u32(completion.data + 40);
            const uint32_t prop_count = read_u32(completion.data + 44);
            const uint32_t encoder_count = read_u32(completion.data + 48);
            if (!large && mode_count <= expected.output_capacity[0] &&
                expected.output_capacity[0])
                memcpy(wire.modes, output + expected.output_offset[0],
                    mode_count * sizeof(wire.modes[0]));
            for (size_t i = 0; !large && prop_count <= expected.output_capacity[1] &&
                i < prop_count; ++i) {
                const unsigned char *property = output +
                    expected.output_offset[1] +
                    i * KB2_GPU_DRM_MODE_RECORD_PROPERTY_VALUE_SIZE;
                if (read_u32(property + 4))
                    return -EPROTO;
                wire.props[i] = read_u32(property);
                wire.prop_values[i] = read_u64(property + 8);
            }
            if (!large && encoder_count <= expected.output_capacity[2] &&
                expected.output_capacity[2])
                memcpy(wire.encoders, output + expected.output_offset[2],
                    encoder_count * sizeof(wire.encoders[0]));
            wire.value.encoder_id = read_u32(completion.data + 8);
            wire.value.connector_type = read_u32(completion.data + 16);
            wire.value.connector_type_id = read_u32(completion.data + 20);
            wire.value.connection = read_u32(completion.data + 24);
            wire.value.mm_width = read_u32(completion.data + 28);
            wire.value.mm_height = read_u32(completion.data + 32);
            wire.value.subpixel = read_u32(completion.data + 36);
            wire.value.count_modes = mode_count;
            wire.value.count_props = prop_count;
            wire.value.count_encoders = encoder_count;
            wire.value.pad = 0;
            memcpy(request->data, &wire,
                large ? sizeof(wire.value) : sizeof(wire));
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
        case KB2_GPU_DRM_MODE_COMMAND_CREATE_PROPERTY_BLOB:
            if (completion.record_schema_id !=
                    KB2_GPU_DRM_MODE_RECORD_OBJECT_ID_RESULT ||
                completion.length !=
                    KB2_GPU_DRM_MODE_RECORD_OBJECT_ID_RESULT_SIZE ||
                read_u64(completion.data) != 1 ||
                !read_u32(completion.data + 8) ||
                read_u32(completion.data + 12))
                return -EPROTO;
            if (expected.command_id ==
                    KB2_GPU_DRM_MODE_COMMAND_CREATE_PROPERTY_BLOB)
                memcpy(request->data +
                    offsetof(gpud_drm_mode_create_blob_wire_t, blob_id),
                    completion.data + 8, sizeof(uint32_t));
            else
                memcpy(request->data, completion.data + 8, sizeof(uint32_t));
            return 0;
        case KB2_GPU_DRM_MODE_COMMAND_GET_PLANE_RESOURCES: {
            gpud_drm_kms_plane_res_wire_t wire;
            if (completion.record_schema_id !=
                    KB2_GPU_DRM_MODE_RECORD_TOPOLOGY_COUNT_RESULT ||
                completion.length != KB2_GPU_DRM_MODE_RECORD_TOPOLOGY_COUNT_RESULT_SIZE ||
                read_u64(completion.data) != 1 ||
                (!output && expected.region.length) ||
                output_size < expected.region.length) return -EPROTO;
            memcpy(&wire, request->data, sizeof(wire));
            uint32_t count = read_u32(completion.data + 8);
            uint32_t required = read_u32(completion.data + 12);
            if (count != (required < expected.output_capacity[0] ?
                required : expected.output_capacity[0])) return -EPROTO;
            if (count) memcpy(wire.planes, output, count * sizeof(uint32_t));
            wire.value.count_planes = required;
            memcpy(request->data, &wire, sizeof(wire));
            return 0;
        }
        case KB2_GPU_DRM_MODE_COMMAND_GET_PLANE: {
            gpud_drm_kms_plane_wire_t wire;
            if (completion.record_schema_id != KB2_GPU_DRM_MODE_RECORD_PLANE_RESULT ||
                completion.length != KB2_GPU_DRM_MODE_RECORD_PLANE_RESULT_SIZE ||
                read_u64(completion.data) != 1 ||
                (!output && expected.region.length) ||
                output_size < expected.region.length) return -EPROTO;
            memcpy(&wire, request->data, sizeof(wire));
            if (read_u32(completion.data + 8) != wire.value.plane_id ||
                read_u32(completion.data + 32)) return -EPROTO;
            uint32_t required = read_u32(completion.data + 28);
            uint32_t count = required <= expected.output_capacity[0] ? required : 0;
            if (count) memcpy(wire.formats, output, count * sizeof(uint32_t));
            wire.value.crtc_id = read_u32(completion.data + 12);
            wire.value.fb_id = read_u32(completion.data + 16);
            wire.value.possible_crtcs = read_u32(completion.data + 20);
            wire.value.gamma_size = read_u32(completion.data + 24);
            wire.value.count_format_types = required;
            memcpy(request->data, &wire, sizeof(wire));
            return 0;
        }
        case KB2_GPU_DRM_MODE_COMMAND_GET_PROPERTY: {
            gpud_drm_kms_property_wire_t wire;
            if (completion.record_schema_id != KB2_GPU_DRM_MODE_RECORD_PROPERTY_RESULT ||
                completion.length != KB2_GPU_DRM_MODE_RECORD_PROPERTY_RESULT_SIZE ||
                read_u64(completion.data) != 1 || read_u32(completion.data + 20) ||
                (!output && expected.region.length) ||
                output_size < expected.region.length) return -EPROTO;
            memcpy(&wire, request->data, sizeof(wire));
            uint32_t values = read_u32(completion.data + 12);
            uint32_t enums = read_u32(completion.data + 16);
            uint32_t value_count = values < expected.output_capacity[0] ?
                values : expected.output_capacity[0];
            uint32_t enum_count = enums < expected.output_capacity[1] ?
                enums : expected.output_capacity[1];
            if (value_count) memcpy(wire.values,
                output + expected.output_offset[0], value_count * sizeof(uint64_t));
            if (enum_count) memcpy(wire.enums,
                output + expected.output_offset[1],
                enum_count * sizeof(wire.enums[0]));
            wire.value.flags = read_u32(completion.data + 8);
            wire.value.count_values = values;
            wire.value.count_enum_blobs = enums;
            memcpy(wire.value.name, completion.data + 24,
                sizeof(wire.value.name));
            memcpy(request->data, &wire, sizeof(wire));
            return 0;
        }
        case KB2_GPU_DRM_MODE_COMMAND_GET_PROPERTY_BLOB: {
            gpud_drm_kms_property_blob_wire_t wire;
            const int large = request->data_size == sizeof(wire.value);
            if (completion.record_schema_id != KB2_GPU_DRM_MODE_RECORD_LENGTH_RESULT ||
                completion.length != KB2_GPU_DRM_MODE_RECORD_LENGTH_RESULT_SIZE ||
                (large ? request->aux_size != expected.region.length :
                    ((!output && expected.region.length) ||
                     output_size < expected.region.length))) return -EPROTO;
            memset(&wire, 0, sizeof(wire));
            memcpy(&wire, request->data,
                large ? sizeof(wire.value) : sizeof(wire));
            uint32_t copied = read_u32(completion.data);
            uint32_t required = read_u32(completion.data + 4);
            if (copied != (required == expected.output_capacity[0] ?
                required : 0)) return -EPROTO;
            if (copied && !large) memcpy(wire.data, output, copied);
            wire.value.length = required;
            memcpy(request->data, &wire,
                large ? sizeof(wire.value) : sizeof(wire));
            return 0;
        }
        case KB2_GPU_DRM_MODE_COMMAND_ATOMIC:
            return completion.record_schema_id == KB2_GPU_DRM_MODE_RECORD_ATOMIC_RESULT &&
                completion.length == KB2_GPU_DRM_MODE_RECORD_ATOMIC_RESULT_SIZE &&
                read_u64(completion.data) == 1 &&
                read_u32(completion.data + 8) == 0 &&
                read_u32(completion.data + 12) == 0 ? 0 : -EPROTO;
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
    if (expected.command_set_id == KB2_GPU_DRM_AMDGPU_SET_ID &&
        expected.command_id == KB2_GPU_DRM_AMDGPU_COMMAND_INFO) {
        uint32_t query = read_u32(request->data + 12);
        uint32_t selectors[4];
        struct kb2_amdgpu_info_shape shape;
        for (unsigned int i = 0; i < 4; ++i)
            selectors[i] = read_u32(request->data + 16 + i * 4);
        if (!kb2_amdgpu_info_shape(query, selectors, &shape) ||
            request->aux_size != shape.bytes) return -EPROTO;
        uint32_t record = shape.record, count = shape.count;
        if (completion.record_schema_id != KB2_GPU_DRM_AMDGPU_RECORD_INFO_RESULT ||
            completion.length != KB2_GPU_DRM_AMDGPU_RECORD_INFO_RESULT_SIZE ||
            read_u32(completion.data +
                KB2_GPU_DRM_AMDGPU_RECORD_INFO_RESULT_RESPONSE_RECORD_ID_OFFSET) != record ||
            read_u32(completion.data +
                KB2_GPU_DRM_AMDGPU_RECORD_INFO_RESULT_RESPONSE_COUNT_OFFSET) != count ||
            read_u32(completion.data +
                KB2_GPU_DRM_AMDGPU_RECORD_INFO_RESULT_RESPONSE_BYTES_OFFSET) != request->aux_size ||
            read_u32(completion.data +
                KB2_GPU_DRM_AMDGPU_RECORD_INFO_RESULT_REQUIRED_COUNT_OFFSET) != count)
            return -EPROTO;
        return 0;
    }
    if (expected.command_set_id == KB2_GPU_DRM_AMDGPU_SET_ID) {
        switch (expected.command_id) {
        case KB2_GPU_DRM_AMDGPU_COMMAND_GEM_OP: {
            const int get_info = read_u32(request->data + 4) == 0;
            if (completion.record_schema_id != KB2_GPU_DRM_AMDGPU_RECORD_GEM_OP_RESULT ||
                completion.length != KB2_GPU_DRM_AMDGPU_RECORD_GEM_OP_RESULT_SIZE ||
                read_u32(completion.data) != (get_info ?
                    KB2_GPU_DRM_AMDGPU_RECORD_GEM_CREATE_INFO : 0) ||
                read_u32(completion.data + 4) != (uint32_t)get_info ||
                read_u32(completion.data + 8) != (uint32_t)get_info ||
                read_u32(completion.data + 12) || request->aux_size != (get_info ? 32u : 0u))
                return -EPROTO;
            return 0;
        }
        case KB2_GPU_DRM_AMDGPU_COMMAND_GEM_CREATE:
            if (completion.record_schema_id !=
                    KB2_GPU_DRM_AMDGPU_RECORD_GEM_HANDLE_RESULT ||
                completion.length !=
                    KB2_GPU_DRM_AMDGPU_RECORD_GEM_HANDLE_RESULT_SIZE ||
                !read_u32(completion.data) || read_u32(completion.data + 4))
                return -EPROTO;
            memcpy(request->data, completion.data, sizeof(uint32_t));
            return 0;
        case KB2_GPU_DRM_AMDGPU_COMMAND_CONTEXT:
            if (completion.record_schema_id !=
                    KB2_GPU_DRM_AMDGPU_RECORD_CONTEXT_RESULT ||
                completion.length !=
                    KB2_GPU_DRM_AMDGPU_RECORD_CONTEXT_RESULT_SIZE ||
                read_u32(completion.data) != read_u32(request->data) ||
                (read_u32(request->data) == 1 &&
                 !read_u32(completion.data + 4)))
                return -EPROTO;
            if (read_u32(request->data) == 1)
                memcpy(request->data, completion.data + 4, sizeof(uint32_t));

            else if (read_u32(request->data) == 3 || read_u32(request->data) == 4)
                memcpy(request->data, completion.data + 8, 16);
            return 0;
        case KB2_GPU_DRM_AMDGPU_COMMAND_GEM_METADATA:
            if (completion.record_schema_id != KB2_GPU_DRM_AMDGPU_RECORD_GEM_METADATA_RESULT ||
                completion.length != KB2_GPU_DRM_AMDGPU_RECORD_GEM_METADATA_RESULT_SIZE ||
                read_u32(completion.data + 16) > 256 || read_u32(completion.data + 20))
                return -EPROTO;
            memcpy(request->data + 8, completion.data, 20);
            memset(request->data + 28, 0, 260);
            memcpy(request->data + 28, completion.data + 24, read_u32(completion.data + 16));
            return 0;
        case KB2_GPU_DRM_AMDGPU_COMMAND_GEM_WAIT_IDLE:
            if (completion.record_schema_id != KB2_GPU_DRM_AMDGPU_RECORD_GEM_WAIT_IDLE_RESULT ||
                completion.length != KB2_GPU_DRM_AMDGPU_RECORD_GEM_WAIT_IDLE_RESULT_SIZE ||
                read_u32(completion.data) > 1) return -EPROTO;
            memcpy(request->data, completion.data, 8);
            return 0;
        case KB2_GPU_DRM_AMDGPU_COMMAND_CS:
            if (completion.record_schema_id !=
                    KB2_GPU_DRM_AMDGPU_RECORD_CS_RESULT ||
                completion.length != KB2_GPU_DRM_AMDGPU_RECORD_CS_RESULT_SIZE ||
                !read_u64(completion.data))
                return -EPROTO;
            memcpy(request->data, completion.data, sizeof(uint64_t));
            return 0;
        case KB2_GPU_DRM_AMDGPU_COMMAND_GEM_VA:
            return completion.record_schema_id || completion.length ?
                -EPROTO : 0;
        case KB2_GPU_DRM_AMDGPU_COMMAND_WAIT_CS:
            if (completion.record_schema_id !=
                    KB2_GPU_DRM_AMDGPU_RECORD_WAIT_STATUS_RESULT ||
                completion.length !=
                    KB2_GPU_DRM_AMDGPU_RECORD_WAIT_STATUS_RESULT_SIZE ||
                read_u32(completion.data) > 1 ||
                read_u32(completion.data + 4))
                return -EPROTO;
            memset(request->data, 0, sizeof(uint64_t));
            memcpy(request->data, completion.data, sizeof(uint32_t));
            return 0;
        case KB2_GPU_DRM_AMDGPU_COMMAND_VM:
            if (completion.record_schema_id !=
                    KB2_GPU_DRM_AMDGPU_RECORD_VM_RESULT ||
                completion.length !=
                    KB2_GPU_DRM_AMDGPU_RECORD_VM_RESULT_SIZE)
                return -EPROTO;
            memcpy(request->data, completion.data, sizeof(uint64_t));
            return 0;
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
