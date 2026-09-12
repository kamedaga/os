/* SPDX-License-Identifier: MIT */
#include "drm_translate.h"

#include <errno.h>
#include <string.h>

/* Exact existing x86-64 Linux ioctl values accepted by LPR. Do not select by
 * ordinal alone: direction and native argument size are part of that ABI. */
#define IOCTL_VERSION UINT32_C(0xc0406400)
#define IOCTL_GET_CAP UINT32_C(0xc010640c)
#define IOCTL_SET_CLIENT_CAP UINT32_C(0x4010640d)
#define IOCTL_GEM_CLOSE UINT32_C(0x40086409)

static uint32_t read_u32(const unsigned char *bytes) {
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
        (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static uint64_t read_u64(const unsigned char *bytes) {
    return read_u32(bytes) | (uint64_t)read_u32(bytes + 4) << 32;
}

static void write_u32(unsigned char *bytes, uint32_t value) {
    for (unsigned int i = 0; i < 4; ++i) bytes[i] = value >> (i * 8);
}

static void write_u64(unsigned char *bytes, uint64_t value) {
    write_u32(bytes, value);
    write_u32(bytes + 4, value >> 32);
}

struct command_plan {
    uint32_t id, record;
    size_t size, span_count;
    unsigned char data[16];
    kb2_gpu_argument_t arguments[4];
    kb2_gpu_span_t spans[3];
};

static int version_plan(struct command_plan *plan, struct gpud_drm_translation *out,
    const drmd_ioctl_request_t *request, uint32_t region_id) {
    drmd_version_wire_t version;
    if (request->arg_size != 64 || request->data_size != sizeof(version))
        return -EINVAL;
    memcpy(&version, request->data, sizeof(version));
    if (version.reserved0) return -EINVAL;
    const uint64_t requested[] = {version.name_capacity, version.date_capacity, version.desc_capacity};
    const uint32_t maximum[] = {DRMD_VERSION_NAME_BYTES, DRMD_VERSION_DATE_BYTES, DRMD_VERSION_DESC_BYTES};
    const uint32_t offsets[] = {0, DRMD_VERSION_NAME_BYTES, DRMD_VERSION_NAME_BYTES + DRMD_VERSION_DATE_BYTES};
    plan->id = KB2_GPU_DRM_CORE_COMMAND_VERSION;
    plan->record = KB2_GPU_DRM_CORE_RECORD_VERSION_REQUEST;
    plan->size = KB2_GPU_DRM_CORE_RECORD_VERSION_REQUEST_SIZE;
    for (size_t i = 0; i < 3; ++i) {
        /* LPR's existing fixed reply fields bound the request, even when the
         * application supplied a larger capacity. Preserve zero-capacity
         * length queries; lengths in the reply need not equal copied bytes. */
        uint32_t capacity = requested[i] < maximum[i] ? (uint32_t)requested[i] : maximum[i];
        out->version_capacity[i] = capacity;
        write_u32(plan->data + 4 * i, capacity);
        if (!capacity) continue; /* Canonical optional spans are omitted, not zero-length. */
        if (!region_id) return -EINVAL;
        out->output_region = (kb2_gpu_region_t){.region_id = region_id,
            .rights = KB2_GPU_SPAN_RIGHT_WRITE, .length = GPUD_DRM_VERSION_BYTES};
        plan->arguments[plan->span_count + 1] = (kb2_gpu_argument_t){.argument_id = i + 2,
            .kind = KB2_GPU_ARGUMENT_SPAN, .flags = KB2_GPU_ARGUMENT_FLAG_OUTPUT,
            .record_schema_id = KB2_GPU_DRM_CORE_RECORD_BYTE, .value = i + 1, .count = capacity};
        plan->spans[plan->span_count++] = (kb2_gpu_span_t){.span_id = i + 1, .region_id = region_id,
            .offset = offsets[i], .length = capacity, .rights = KB2_GPU_SPAN_RIGHT_WRITE,
            .record_schema_id = KB2_GPU_DRM_CORE_RECORD_BYTE, .element_count = capacity,
            .flags = KB2_GPU_SPAN_FLAG_OUTPUT};
    }
    return 0;
}

static int scalar_plan(struct command_plan *plan, const drmd_ioctl_request_t *request) {
    switch (request->request) {
    case IOCTL_GET_CAP:
        if (request->arg_size != 16 || request->data_size != 16) return -EINVAL;
        plan->id = KB2_GPU_DRM_CORE_COMMAND_GET_CAP;
        plan->record = KB2_GPU_DRM_CORE_RECORD_CAP_REQUEST;
        plan->size = KB2_GPU_DRM_CORE_RECORD_CAP_REQUEST_SIZE;
        write_u64(plan->data, read_u64(request->data));
        return 0;
    case IOCTL_SET_CLIENT_CAP:
        if (request->arg_size != 16 || request->data_size != 16) return -EINVAL;
        plan->id = KB2_GPU_DRM_CORE_COMMAND_SET_CLIENT_CAP;
        plan->record = KB2_GPU_DRM_CORE_RECORD_CLIENT_CAP_REQUEST;
        plan->size = KB2_GPU_DRM_CORE_RECORD_CLIENT_CAP_REQUEST_SIZE;
        write_u64(plan->data, read_u64(request->data));
        write_u64(plan->data + 8, read_u64(request->data + 8));
        return 0;
    case IOCTL_GEM_CLOSE:
        if (request->arg_size != 8 || request->data_size != 8 || read_u32(request->data + 4))
            return -EINVAL;
        plan->id = KB2_GPU_DRM_CORE_COMMAND_GEM_CLOSE;
        plan->record = KB2_GPU_DRM_CORE_RECORD_GEM_HANDLE_REQUEST;
        plan->size = KB2_GPU_DRM_CORE_RECORD_GEM_HANDLE_REQUEST_SIZE;
        write_u32(plan->data, read_u32(request->data));
        return 0;
    default:
        return -EOPNOTSUPP;
    }
}

int gpud_drm_ioctl_encode(struct gpud_drm_translation *out,
    const struct gpud_drm_binding *binding, const drmd_ioctl_request_t *request,
    uint32_t output_region_id) {
    if (!out || !binding || !request || !binding->generation || !binding->frontend_handle ||
        !binding->session_id || request->request > UINT32_MAX || request->reserved0)
        return -EINVAL;
    if (request->handle != binding->frontend_handle) return -EACCES;
    if (request->aux_size || request->fd_flags) return -EOPNOTSUPP;
    struct gpud_drm_translation candidate = {.generation = binding->generation,
        .frontend_handle = binding->frontend_handle, .session_id = binding->session_id};
    struct command_plan plan = {0};
    int result = request->request == IOCTL_VERSION ?
        version_plan(&plan, &candidate, request, output_region_id) : scalar_plan(&plan, request);
    if (result) return result;
    plan.arguments[0] = (kb2_gpu_argument_t){.argument_id = KB2_GPU_DRM_CORE_INLINE_ARGUMENT_ID,
        .kind = KB2_GPU_ARGUMENT_INLINE, .flags = KB2_GPU_ARGUMENT_FLAG_INPUT,
        .record_schema_id = plan.record, .count = plan.size};
    kb2_gpu_command_source_t source = {.generation = binding->generation,
        .session_id = binding->session_id, .profile_kind = KB2_GPU_PROFILE_VIRGL,
        .queue_class = KB2_GPU_QUEUE_EXECUTION, .command_set_id = KB2_GPU_DRM_CORE_SET_ID,
        .command_id = plan.id, .arguments = plan.arguments, .argument_count = 1 + plan.span_count,
        .spans = plan.spans, .span_count = plan.span_count, .inline_data = plan.data,
        .inline_length = plan.size, .regions = &candidate.output_region,
        .region_count = plan.span_count ? 1 : 0};
    kb2_protocol_status_t status = kb2_gpu_command_encode(candidate.command, sizeof(candidate.command),
        &candidate.command_size, &source);
    if (status != KB2_PROTOCOL_OK) return -EINVAL;
    candidate.command_id = plan.id;
    candidate.queue_class = source.queue_class;
    *out = candidate;
    return 0;
}
