/* SPDX-License-Identifier: MIT */
#include "drm_reply.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

static uint32_t read_u32(const unsigned char *bytes) {
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
        (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
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

int gpud_drm_ioctl_reply(drmd_ioctl_request_t *request,
    const struct gpud_drm_translation *translation, uint64_t correlation,
    const unsigned char *reply, size_t reply_size,
    const unsigned char *output, size_t output_size) {
    if (!request || !translation || !correlation || !reply) return -EINVAL;
    /* Bind output conversion to the exact request admitted by the owner. */
    struct gpud_drm_translation expected;
    struct gpud_drm_binding binding = {.generation = translation->generation,
        .frontend_handle = translation->frontend_handle, .session_id = translation->session_id};
    if (gpud_drm_ioctl_encode(&expected, &binding, request, translation->output_region.region_id) ||
        expected.command_size != translation->command_size ||
        expected.command_id != translation->command_id ||
        memcmp(expected.command, translation->command, expected.command_size)) return -EPROTO;
    kb2_protocol_message_envelope_t envelope;
    if (kb2_protocol_message_envelope_decode(reply, reply_size, &envelope) ||
        envelope.protocol_id != KB2_GPU_PROTOCOL_ID || envelope.opcode != KB2_GPU_OPCODE_COMMAND ||
        envelope.flags || envelope.generation != binding.generation || envelope.correlation_id != correlation ||
        envelope.payload_length != reply_size - KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE) return -EPROTO;
    kb2_gpu_inline_completion_t completion;
    if (kb2_gpu_inline_completion_decode(reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
        envelope.payload_length, binding.session_id, &completion)) return -EPROTO;
    int result = gpud_drm_status_errno(completion.status);
    if (result) return result;
    switch (expected.command_id) {
    case KB2_GPU_DRM_CORE_COMMAND_VERSION: {
        if (completion.record_schema_id != KB2_GPU_DRM_CORE_RECORD_VERSION_RESULT ||
            completion.length != KB2_GPU_DRM_CORE_RECORD_VERSION_RESULT_SIZE ||
            read_u32(completion.data + 12) || read_u32(completion.data + 28) ||
            read_u32(completion.data) > INT32_MAX || read_u32(completion.data + 4) > INT32_MAX ||
            read_u32(completion.data + 8) > INT32_MAX) return -EPROTO;
        if (expected.output_region.region_id && (!output || output_size < GPUD_DRM_VERSION_BYTES))
            return -EMSGSIZE;
        drmd_version_wire_t version;
        memcpy(&version, request->data, sizeof(version));
        version.major = (int32_t)read_u32(completion.data);
        version.minor = (int32_t)read_u32(completion.data + 4);
        version.patchlevel = (int32_t)read_u32(completion.data + 8);
        version.name_length = read_u32(completion.data + 16);
        version.date_length = read_u32(completion.data + 20);
        version.desc_length = read_u32(completion.data + 24);
        char *strings[] = {version.name, version.date, version.desc};
        const uint64_t lengths[] = {version.name_length, version.date_length, version.desc_length};
        const size_t offsets[] = {0, DRMD_VERSION_NAME_BYTES, DRMD_VERSION_NAME_BYTES + DRMD_VERSION_DATE_BYTES};
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
        return completion.length || completion.record_schema_id ? -EPROTO : 0;
    default:
        return -EOPNOTSUPP;
    }
}
