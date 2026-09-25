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
#define IOCTL_SYNCOBJ_CREATE UINT32_C(0xc00864bf)
#define IOCTL_SYNCOBJ_DESTROY UINT32_C(0xc00864c0)
#define IOCTL_SYNCOBJ_WAIT UINT32_C(0xc02864c3)
#define IOCTL_SYNCOBJ_RESET UINT32_C(0xc01064c4)
#define IOCTL_SYNCOBJ_SIGNAL UINT32_C(0xc01064c5)
#define IOCTL_SET_MASTER UINT32_C(0x641e)
#define IOCTL_DROP_MASTER UINT32_C(0x641f)
#define IOCTL_VIRTGPU_MAP UINT32_C(0xc0106441)
#define IOCTL_VIRTGPU_EXECBUFFER UINT32_C(0xc0406442)
#define IOCTL_VIRTGPU_GETPARAM UINT32_C(0xc0106443)
#define IOCTL_VIRTGPU_RESOURCE_CREATE UINT32_C(0xc0386444)
#define IOCTL_VIRTGPU_RESOURCE_INFO UINT32_C(0xc0106445)
#define IOCTL_VIRTGPU_TRANSFER_FROM_HOST UINT32_C(0xc02c6446)
#define IOCTL_VIRTGPU_TRANSFER_TO_HOST UINT32_C(0xc02c6447)
#define IOCTL_VIRTGPU_WAIT UINT32_C(0xc0086448)
#define IOCTL_VIRTGPU_CONTEXT_INIT UINT32_C(0xc010644b)

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

static void write_u16(unsigned char *bytes, uint16_t value) {
    bytes[0] = value;
    bytes[1] = value >> 8;
}

static void write_u64(unsigned char *bytes, uint64_t value) {
    write_u32(bytes, value);
    write_u32(bytes + 4, value >> 32);
}

struct command_plan {
    uint32_t set, id, record;
    size_t size, span_count, attachment_count;
    uint64_t deadline_ns;
    unsigned char data[KB2_GPU_DRM_MODE_RECORD_FB2_SIZE];
    kb2_gpu_argument_t arguments[5];
    kb2_gpu_span_t spans[4];
    kb2_gpu_attachment_t attachments[1];
};

static int version_plan(struct command_plan *plan, struct gpud_drm_translation *out,
    const gpud_drm_ioctl_request_t *request, uint32_t region_id) {
    gpud_drm_version_wire_t version;
    if (request->arg_size != 64 || request->data_size != sizeof(version))
        return -EINVAL;
    memcpy(&version, request->data, sizeof(version));
    if (version.reserved0) return -EINVAL;
    const uint64_t requested[] = {version.name_capacity, version.date_capacity, version.desc_capacity};
    const uint32_t maximum[] = {GPUD_DRM_VERSION_NAME_BYTES, GPUD_DRM_VERSION_DATE_BYTES, GPUD_DRM_VERSION_DESC_BYTES};
    const uint32_t offsets[] = {0, GPUD_DRM_VERSION_NAME_BYTES, GPUD_DRM_VERSION_NAME_BYTES + GPUD_DRM_VERSION_DATE_BYTES};
    plan->set = KB2_GPU_DRM_CORE_SET_ID;
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
        out->region = (kb2_gpu_region_t){.region_id = region_id,
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

static int scalar_plan(struct command_plan *plan, const gpud_drm_ioctl_request_t *request) {
    plan->set = KB2_GPU_DRM_CORE_SET_ID;
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
    case IOCTL_SYNCOBJ_CREATE:
        if (request->arg_size != sizeof(gpud_drm_syncobj_create_t) ||
            request->data_size != sizeof(gpud_drm_syncobj_create_t) ||
            read_u32(request->data) || (read_u32(request->data + 4) & ~1u))
            return -EINVAL;
        plan->id = KB2_GPU_DRM_CORE_COMMAND_SYNCOBJ_CREATE;
        plan->record = KB2_GPU_DRM_CORE_RECORD_SYNCOBJ_CREATE_REQUEST;
        plan->size = KB2_GPU_DRM_CORE_RECORD_SYNCOBJ_CREATE_REQUEST_SIZE;
        write_u32(plan->data, read_u32(request->data + 4));
        return 0;
    case IOCTL_SYNCOBJ_DESTROY:
        if (request->arg_size != sizeof(gpud_drm_syncobj_destroy_t) ||
            request->data_size != sizeof(gpud_drm_syncobj_destroy_t) ||
            !read_u32(request->data) || read_u32(request->data + 4))
            return -EINVAL;
        plan->id = KB2_GPU_DRM_CORE_COMMAND_SYNCOBJ_DESTROY;
        plan->record = KB2_GPU_DRM_CORE_RECORD_GEM_HANDLE_REQUEST;
        plan->size = KB2_GPU_DRM_CORE_RECORD_GEM_HANDLE_REQUEST_SIZE;
        write_u32(plan->data, read_u32(request->data));
        return 0;
    default:
        return -EOPNOTSUPP;
    }
}

static int mode_plan(struct command_plan *plan,
    struct gpud_drm_translation *out,
    const gpud_drm_ioctl_request_t *request, uint32_t region_id) {
    plan->set = KB2_GPU_DRM_MODE_SET_ID;
    switch (request->request) {
    case GPUD_DRM_IOCTL_GET_MAGIC:
        if (request->arg_size != 4 || request->data_size != 4)
            return -EINVAL;
        plan->id = KB2_GPU_DRM_MODE_COMMAND_GET_MAGIC;
        return 0;
    case GPUD_DRM_IOCTL_AUTH_MAGIC:
        if (request->arg_size != 4 || request->data_size != 4)
            return -EINVAL;
        plan->id = KB2_GPU_DRM_MODE_COMMAND_AUTH_MAGIC;
        plan->record = KB2_GPU_DRM_MODE_RECORD_MAGIC;
        plan->size = KB2_GPU_DRM_MODE_RECORD_MAGIC_SIZE;
        write_u32(plan->data, read_u32(request->data));
        return 0;
    case IOCTL_SET_MASTER:
        if (request->arg_size || request->data_size)
            return -EINVAL;
        plan->id = KB2_GPU_DRM_MODE_COMMAND_SET_MASTER;
        return 0;
    case IOCTL_DROP_MASTER:
        if (request->arg_size || request->data_size)
            return -EINVAL;
        plan->id = KB2_GPU_DRM_MODE_COMMAND_DROP_MASTER;
        return 0;
    case GPUD_DRM_IOCTL_MODE_GETRESOURCES: {
        gpud_drm_kms_resources_wire_t wire;
        const uint32_t limits[4] = {
            GPUD_DRM_KMS_FB_CAPACITY, GPUD_DRM_KMS_CRTC_CAPACITY,
            GPUD_DRM_KMS_CONNECTOR_CAPACITY,
            GPUD_DRM_KMS_ENCODER_CAPACITY};
        if (request->arg_size != sizeof(wire.value) ||
            request->data_size != sizeof(wire) || !region_id)
            return -EINVAL;
        memcpy(&wire, request->data, sizeof(wire));
        const uint32_t requested[4] = {wire.value.count_fbs,
            wire.value.count_crtcs, wire.value.count_connectors,
            wire.value.count_encoders};
        plan->id = KB2_GPU_DRM_MODE_COMMAND_GET_RESOURCES;
        plan->record = KB2_GPU_DRM_MODE_RECORD_RESOURCES_REQUEST;
        plan->size = KB2_GPU_DRM_MODE_RECORD_RESOURCES_REQUEST_SIZE;
        uint32_t offset = 0;
        for (size_t i = 0; i < 4; ++i) {
            uint32_t capacity = requested[i] < limits[i] ?
                requested[i] : limits[i];
            out->output_capacity[i] = capacity;
            out->output_offset[i] = offset;
            write_u32(plan->data + 4 * i, capacity);
            if (capacity) {
                plan->arguments[plan->span_count + 1] =
                    (kb2_gpu_argument_t){.argument_id = i + 2,
                        .kind = KB2_GPU_ARGUMENT_SPAN,
                        .flags = KB2_GPU_ARGUMENT_FLAG_OUTPUT,
                        .record_schema_id = KB2_GPU_DRM_MODE_RECORD_OBJECT_ID,
                        .value = i + 1, .count = capacity};
                plan->spans[plan->span_count++] = (kb2_gpu_span_t){
                    .span_id = i + 1, .region_id = region_id,
                    .offset = offset, .length = (uint64_t)capacity * 4,
                    .rights = KB2_GPU_SPAN_RIGHT_WRITE,
                    .record_schema_id = KB2_GPU_DRM_MODE_RECORD_OBJECT_ID,
                    .element_count = capacity,
                    .flags = KB2_GPU_SPAN_FLAG_OUTPUT};
                offset += capacity * 4;
            }
        }
        out->region = (kb2_gpu_region_t){.region_id = region_id,
            .rights = KB2_GPU_SPAN_RIGHT_WRITE, .length = offset};
        return 0;
    }
    case GPUD_DRM_IOCTL_MODE_GETCONNECTOR: {
        gpud_drm_kms_connector_wire_t wire;
        const uint32_t limits[3] = {GPUD_DRM_KMS_MODE_CAPACITY,
            GPUD_DRM_KMS_PROPERTY_CAPACITY,
            GPUD_DRM_KMS_ENCODER_CAPACITY};
        const uint32_t records[3] = {KB2_GPU_DRM_MODE_RECORD_MODE_INFO,
            KB2_GPU_DRM_MODE_RECORD_PROPERTY_VALUE,
            KB2_GPU_DRM_MODE_RECORD_OBJECT_ID};
        const uint32_t sizes[3] = {KB2_GPU_DRM_MODE_RECORD_MODE_INFO_SIZE,
            KB2_GPU_DRM_MODE_RECORD_PROPERTY_VALUE_SIZE,
            KB2_GPU_DRM_MODE_RECORD_OBJECT_ID_SIZE};
        if (request->arg_size != sizeof(wire.value) ||
            request->data_size != sizeof(wire))
            return -EINVAL;
        memcpy(&wire, request->data, sizeof(wire));
        if (!wire.value.connector_id || wire.value.pad)
            return -EINVAL;
        const uint32_t requested[3] = {wire.value.count_modes,
            wire.value.count_props, wire.value.count_encoders};
        plan->id = KB2_GPU_DRM_MODE_COMMAND_GET_CONNECTOR;
        plan->record = KB2_GPU_DRM_MODE_RECORD_CONNECTOR_REQUEST;
        plan->size = KB2_GPU_DRM_MODE_RECORD_CONNECTOR_REQUEST_SIZE;
        write_u64(plan->data, 1);
        write_u32(plan->data + 8, wire.value.connector_id);
        write_u32(plan->data + 24, requested[0] ? 0 :
            KB2_GPU_DRM_MODE_CONNECTOR_FLAG_FORCE_PROBE);
        uint32_t offset = 0;
        for (size_t i = 0; i < 3; ++i) {
            uint32_t capacity = requested[i] < limits[i] ?
                requested[i] : limits[i];
            out->output_capacity[i] = capacity;
            out->output_offset[i] = offset;
            write_u32(plan->data + 12 + 4 * i, capacity);
            if (!capacity)
                continue;
            if (!region_id)
                return -EINVAL;
            plan->arguments[plan->span_count + 1] =
                (kb2_gpu_argument_t){.argument_id = i + 2,
                    .kind = KB2_GPU_ARGUMENT_SPAN,
                    .flags = KB2_GPU_ARGUMENT_FLAG_OUTPUT,
                    .record_schema_id = records[i],
                    .value = i + 1, .count = capacity};
            plan->spans[plan->span_count++] = (kb2_gpu_span_t){
                .span_id = i + 1, .region_id = region_id,
                .offset = offset, .length = (uint64_t)capacity * sizes[i],
                .rights = KB2_GPU_SPAN_RIGHT_WRITE,
                .record_schema_id = records[i],
                .element_count = capacity,
                .flags = KB2_GPU_SPAN_FLAG_OUTPUT};
            offset += capacity * sizes[i];
        }
        out->region = (kb2_gpu_region_t){.region_id = region_id,
            .rights = KB2_GPU_SPAN_RIGHT_WRITE, .length = offset};
        return 0;
    }
    case GPUD_DRM_IOCTL_MODE_GETENCODER: {
        gpud_drm_mode_get_encoder_t encoder;
        if (request->arg_size != sizeof(encoder) ||
            request->data_size != sizeof(encoder))
            return -EINVAL;
        memcpy(&encoder, request->data, sizeof(encoder));
        if (!encoder.encoder_id)
            return -EINVAL;
        plan->id = KB2_GPU_DRM_MODE_COMMAND_GET_ENCODER;
        plan->record = KB2_GPU_DRM_MODE_RECORD_ENCODER_REQUEST;
        plan->size = KB2_GPU_DRM_MODE_RECORD_ENCODER_REQUEST_SIZE;
        write_u64(plan->data, 1);
        write_u32(plan->data + 8, encoder.encoder_id);
        return 0;
    }
    case GPUD_DRM_IOCTL_MODE_ADDFB: {
        gpud_drm_mode_fb_cmd_t framebuffer;
        if (request->arg_size != sizeof(framebuffer) ||
            request->data_size != sizeof(framebuffer))
            return -EINVAL;
        memcpy(&framebuffer, request->data, sizeof(framebuffer));
        if (framebuffer.fb_id || !framebuffer.width || !framebuffer.height ||
            !framebuffer.pitch || !framebuffer.bpp || !framebuffer.depth ||
            !framebuffer.handle)
            return -EINVAL;
        plan->id = KB2_GPU_DRM_MODE_COMMAND_ADD_FB;
        plan->record = KB2_GPU_DRM_MODE_RECORD_FB_LEGACY_CREATE;
        plan->size = KB2_GPU_DRM_MODE_RECORD_FB_LEGACY_CREATE_SIZE;
        write_u64(plan->data +
            KB2_GPU_DRM_MODE_RECORD_FB_LEGACY_CREATE_TOPOLOGY_EPOCH_OFFSET,
            1);
        write_u32(plan->data +
            KB2_GPU_DRM_MODE_RECORD_FB_LEGACY_CREATE_WIDTH_OFFSET,
            framebuffer.width);
        write_u32(plan->data +
            KB2_GPU_DRM_MODE_RECORD_FB_LEGACY_CREATE_HEIGHT_OFFSET,
            framebuffer.height);
        write_u32(plan->data +
            KB2_GPU_DRM_MODE_RECORD_FB_LEGACY_CREATE_PITCH_OFFSET,
            framebuffer.pitch);
        write_u32(plan->data +
            KB2_GPU_DRM_MODE_RECORD_FB_LEGACY_CREATE_BITS_PER_PIXEL_OFFSET,
            framebuffer.bpp);
        write_u32(plan->data +
            KB2_GPU_DRM_MODE_RECORD_FB_LEGACY_CREATE_DEPTH_OFFSET,
            framebuffer.depth);
        write_u32(plan->data +
            KB2_GPU_DRM_MODE_RECORD_FB_LEGACY_CREATE_HANDLE_OFFSET,
            framebuffer.handle);
        return 0;
    }
    case GPUD_DRM_IOCTL_MODE_RMFB:
        if (request->arg_size != sizeof(uint32_t) ||
            request->data_size != sizeof(uint32_t) ||
            !read_u32(request->data))
            return -EINVAL;
        plan->id = KB2_GPU_DRM_MODE_COMMAND_REMOVE_FB;
        plan->record = KB2_GPU_DRM_MODE_RECORD_OBJECT_ID_REQUEST;
        plan->size = KB2_GPU_DRM_MODE_RECORD_OBJECT_ID_REQUEST_SIZE;
        write_u64(plan->data +
            KB2_GPU_DRM_MODE_RECORD_OBJECT_ID_REQUEST_TOPOLOGY_EPOCH_OFFSET,
            1);
        write_u32(plan->data +
            KB2_GPU_DRM_MODE_RECORD_OBJECT_ID_REQUEST_OBJECT_ID_OFFSET,
            read_u32(request->data));
        return 0;
    case GPUD_DRM_IOCTL_MODE_ADDFB2: {
        gpud_drm_mode_fb_cmd2_t framebuffer;
        if (request->arg_size != sizeof(framebuffer) ||
            request->data_size != sizeof(framebuffer))
            return -EINVAL;
        memcpy(&framebuffer, request->data, sizeof(framebuffer));
        if (framebuffer.fb_id || !framebuffer.width || !framebuffer.height ||
            !framebuffer.pixel_format || !framebuffer.handles[0] ||
            !framebuffer.pitches[0] ||
            (framebuffer.flags & ~GPUD_DRM_MODE_FB_MODIFIERS))
            return -EINVAL;
        plan->id = KB2_GPU_DRM_MODE_COMMAND_ADD_FB2;
        plan->record = KB2_GPU_DRM_MODE_RECORD_FB2;
        plan->size = KB2_GPU_DRM_MODE_RECORD_FB2_SIZE;
        write_u64(plan->data, 1);
        write_u32(plan->data + 8, framebuffer.fb_id);
        write_u32(plan->data + 12, framebuffer.width);
        write_u32(plan->data + 16, framebuffer.height);
        write_u32(plan->data + 20, framebuffer.pixel_format);
        write_u32(plan->data + 24, framebuffer.flags);
        for (size_t i = 0; i < 4; ++i) {
            write_u32(plan->data + 28 + i * 4, framebuffer.handles[i]);
            write_u32(plan->data + 44 + i * 4, framebuffer.pitches[i]);
            write_u32(plan->data + 60 + i * 4, framebuffer.offsets[i]);
            write_u64(plan->data + 76 + i * 8, framebuffer.modifier[i]);
        }
        return 0;
    }
    case GPUD_DRM_IOCTL_MODE_OBJ_GETPROPERTIES: {
        gpud_drm_kms_object_properties_wire_t wire;
        if (request->arg_size != sizeof(wire.value) ||
            request->data_size != sizeof(wire))
            return -EINVAL;
        memcpy(&wire, request->data, sizeof(wire));
        if (!wire.value.obj_id || wire.value.pad ||
            (wire.value.obj_type != GPUD_DRM_MODE_OBJECT_CRTC &&
             wire.value.obj_type != GPUD_DRM_MODE_OBJECT_CONNECTOR &&
             wire.value.obj_type != GPUD_DRM_MODE_OBJECT_FB &&
             wire.value.obj_type != GPUD_DRM_MODE_OBJECT_PLANE))
            return -EINVAL;
        uint32_t capacity = wire.value.count_props <
            GPUD_DRM_KMS_PROPERTY_CAPACITY ? wire.value.count_props :
            GPUD_DRM_KMS_PROPERTY_CAPACITY;
        plan->id = KB2_GPU_DRM_MODE_COMMAND_OBJECT_GET_PROPERTIES;
        plan->record = KB2_GPU_DRM_MODE_RECORD_OBJECT_PROPERTIES_REQUEST;
        plan->size = KB2_GPU_DRM_MODE_RECORD_OBJECT_PROPERTIES_REQUEST_SIZE;
        write_u64(plan->data +
            KB2_GPU_DRM_MODE_RECORD_OBJECT_PROPERTIES_REQUEST_TOPOLOGY_EPOCH_OFFSET,
            1);
        write_u32(plan->data +
            KB2_GPU_DRM_MODE_RECORD_OBJECT_PROPERTIES_REQUEST_OBJECT_ID_OFFSET,
            wire.value.obj_id);
        write_u32(plan->data +
            KB2_GPU_DRM_MODE_RECORD_OBJECT_PROPERTIES_REQUEST_OBJECT_TYPE_OFFSET,
            wire.value.obj_type);
        write_u32(plan->data +
            KB2_GPU_DRM_MODE_RECORD_OBJECT_PROPERTIES_REQUEST_PROPERTY_CAPACITY_OFFSET,
            capacity);
        out->output_capacity[0] = capacity;
        if (capacity) {
            if (!region_id)
                return -EINVAL;
            plan->arguments[1] = (kb2_gpu_argument_t) {
                .argument_id = 2,
                .kind = KB2_GPU_ARGUMENT_SPAN,
                .flags = KB2_GPU_ARGUMENT_FLAG_OUTPUT,
                .record_schema_id = KB2_GPU_DRM_MODE_RECORD_PROPERTY_VALUE,
                .value = 1,
                .count = capacity,
            };
            plan->spans[0] = (kb2_gpu_span_t) {
                .span_id = 1,
                .region_id = region_id,
                .length = (uint64_t)capacity *
                    KB2_GPU_DRM_MODE_RECORD_PROPERTY_VALUE_SIZE,
                .rights = KB2_GPU_SPAN_RIGHT_WRITE,
                .record_schema_id = KB2_GPU_DRM_MODE_RECORD_PROPERTY_VALUE,
                .element_count = capacity,
                .flags = KB2_GPU_SPAN_FLAG_OUTPUT,
            };
            plan->span_count = 1;
            out->region = (kb2_gpu_region_t) {
                .region_id = region_id,
                .rights = KB2_GPU_SPAN_RIGHT_WRITE,
                .length = plan->spans[0].length,
            };
        }
        return 0;
    }
    case GPUD_DRM_IOCTL_MODE_CREATE_DUMB: {
        gpud_drm_mode_create_dumb_t buffer;
        if (request->arg_size != sizeof(buffer) ||
            request->data_size != sizeof(buffer))
            return -EINVAL;
        memcpy(&buffer, request->data, sizeof(buffer));
        if (!buffer.height || !buffer.width || !buffer.bpp || buffer.flags)
            return -EINVAL;
        plan->id = KB2_GPU_DRM_MODE_COMMAND_CREATE_DUMB;
        plan->record = KB2_GPU_DRM_MODE_RECORD_DUMB_CREATE_REQUEST;
        plan->size = KB2_GPU_DRM_MODE_RECORD_DUMB_CREATE_REQUEST_SIZE;
        write_u32(plan->data +
            KB2_GPU_DRM_MODE_RECORD_DUMB_CREATE_REQUEST_HEIGHT_OFFSET,
            buffer.height);
        write_u32(plan->data +
            KB2_GPU_DRM_MODE_RECORD_DUMB_CREATE_REQUEST_WIDTH_OFFSET,
            buffer.width);
        write_u32(plan->data +
            KB2_GPU_DRM_MODE_RECORD_DUMB_CREATE_REQUEST_BITS_PER_PIXEL_OFFSET,
            buffer.bpp);
        write_u32(plan->data +
            KB2_GPU_DRM_MODE_RECORD_DUMB_CREATE_REQUEST_FLAGS_OFFSET,
            buffer.flags);
        return 0;
    }
    case GPUD_DRM_IOCTL_MODE_MAP_DUMB: {
        gpud_drm_mode_map_dumb_t mapping;
        if (request->arg_size != sizeof(mapping) ||
            request->data_size != sizeof(mapping))
            return -EINVAL;
        memcpy(&mapping, request->data, sizeof(mapping));
        if (!mapping.handle || mapping.pad)
            return -EINVAL;
        out->mapping_handle = mapping.handle;
        plan->id = KB2_GPU_DRM_MODE_COMMAND_MAP_DUMB;
        plan->record = KB2_GPU_DRM_MODE_RECORD_MAP_REQUEST;
        plan->size = KB2_GPU_DRM_MODE_RECORD_MAP_REQUEST_SIZE;
        write_u32(plan->data +
            KB2_GPU_DRM_MODE_RECORD_MAP_REQUEST_HANDLE_OFFSET,
            mapping.handle);
        write_u32(plan->data +
            KB2_GPU_DRM_MODE_RECORD_MAP_REQUEST_MAPPING_RIGHTS_OFFSET,
            KB2_GPU_SPAN_RIGHT_READ | KB2_GPU_SPAN_RIGHT_WRITE);
        return 0;
    }
    case GPUD_DRM_IOCTL_MODE_GETCRTC: {
        gpud_drm_kms_crtc_wire_t wire;
        if (request->arg_size != sizeof(wire.value) ||
            request->data_size != sizeof(wire) || !region_id)
            return -EINVAL;
        memcpy(&wire, request->data, sizeof(wire));
        if (!wire.value.crtc_id)
            return -EINVAL;
        plan->id = KB2_GPU_DRM_MODE_COMMAND_GET_CRTC;
        plan->record = KB2_GPU_DRM_MODE_RECORD_CRTC_GET_REQUEST;
        plan->size = KB2_GPU_DRM_MODE_RECORD_CRTC_GET_REQUEST_SIZE;
        write_u64(plan->data, 1);
        write_u32(plan->data + 8, wire.value.crtc_id);
        plan->arguments[1] = (kb2_gpu_argument_t){.argument_id = 2,
            .kind = KB2_GPU_ARGUMENT_SPAN,
            .flags = KB2_GPU_ARGUMENT_FLAG_OUTPUT,
            .record_schema_id = KB2_GPU_DRM_MODE_RECORD_MODE_INFO,
            .value = 1, .count = 1};
        plan->spans[0] = (kb2_gpu_span_t){.span_id = 1,
            .region_id = region_id,
            .length = KB2_GPU_DRM_MODE_RECORD_MODE_INFO_SIZE,
            .rights = KB2_GPU_SPAN_RIGHT_WRITE,
            .record_schema_id = KB2_GPU_DRM_MODE_RECORD_MODE_INFO,
            .element_count = 1, .flags = KB2_GPU_SPAN_FLAG_OUTPUT};
        plan->span_count = 1;
        out->output_capacity[0] = 1;
        out->region = (kb2_gpu_region_t){.region_id = region_id,
            .rights = KB2_GPU_SPAN_RIGHT_WRITE,
            .length = KB2_GPU_DRM_MODE_RECORD_MODE_INFO_SIZE};
        return 0;
    }
    case GPUD_DRM_IOCTL_MODE_SETCRTC: {
        gpud_drm_kms_crtc_wire_t wire;
        const size_t mode_offset = (size_t)read_u32(request->data + 8) *
            sizeof(uint32_t);
        if (request->arg_size != sizeof(wire.value) ||
            request->data_size != sizeof(wire) || !region_id)
            return -EINVAL;
        memcpy(&wire, request->data, sizeof(wire));
        if (!wire.value.crtc_id || wire.value.count_connectors >
                GPUD_DRM_KMS_CONNECTOR_CAPACITY ||
            wire.value.mode_valid > 1 ||
            (!!wire.value.count_connectors !=
                !!wire.value.set_connectors_ptr) ||
            mode_offset > sizeof(out->staged_input) ||
            (wire.value.mode_valid && sizeof(wire.value.mode) >
                sizeof(out->staged_input) - mode_offset))
            return -EINVAL;
        for (size_t i = 0; i < wire.value.count_connectors; ++i) {
            if (!wire.connectors[i])
                return -EINVAL;
            write_u32(out->staged_input + i * sizeof(uint32_t),
                wire.connectors[i]);
        }
        if (wire.value.mode_valid) {
            unsigned char *mode = out->staged_input + mode_offset;
            write_u32(mode, wire.value.mode.clock);
            write_u16(mode + 4, wire.value.mode.hdisplay);
            write_u16(mode + 6, wire.value.mode.hsync_start);
            write_u16(mode + 8, wire.value.mode.hsync_end);
            write_u16(mode + 10, wire.value.mode.htotal);
            write_u16(mode + 12, wire.value.mode.hskew);
            write_u16(mode + 14, wire.value.mode.vdisplay);
            write_u16(mode + 16, wire.value.mode.vsync_start);
            write_u16(mode + 18, wire.value.mode.vsync_end);
            write_u16(mode + 20, wire.value.mode.vtotal);
            write_u16(mode + 22, wire.value.mode.vscan);
            write_u32(mode + 24, wire.value.mode.vrefresh);
            write_u32(mode + 28, wire.value.mode.flags);
            write_u32(mode + 32, wire.value.mode.type);
            memcpy(mode + 36, wire.value.mode.name,
                sizeof(wire.value.mode.name));
        }
        out->staged_input_size = mode_offset +
            wire.value.mode_valid * sizeof(wire.value.mode);
        out->region = (kb2_gpu_region_t){.region_id = region_id,
            .rights = KB2_GPU_SPAN_RIGHT_READ,
            .length = out->staged_input_size};
        plan->id = KB2_GPU_DRM_MODE_COMMAND_SET_CRTC;
        plan->record = KB2_GPU_DRM_MODE_RECORD_CRTC_SET_REQUEST;
        plan->size = KB2_GPU_DRM_MODE_RECORD_CRTC_SET_REQUEST_SIZE;
        write_u64(plan->data, 1);
        write_u32(plan->data + 8, wire.value.crtc_id);
        write_u32(plan->data + 12, wire.value.fb_id);
        write_u32(plan->data + 16, wire.value.x);
        write_u32(plan->data + 20, wire.value.y);
        write_u32(plan->data + 24, wire.value.mode_valid);
        write_u32(plan->data + 28, wire.value.count_connectors);
        if (wire.value.count_connectors) {
            plan->arguments[1] = (kb2_gpu_argument_t){.argument_id = 2,
                .kind = KB2_GPU_ARGUMENT_SPAN,
                .flags = KB2_GPU_ARGUMENT_FLAG_INPUT,
                .record_schema_id = KB2_GPU_DRM_MODE_RECORD_OBJECT_ID,
                .value = 1, .count = wire.value.count_connectors};
            plan->spans[0] = (kb2_gpu_span_t){.span_id = 1,
                .region_id = region_id, .length = mode_offset,
                .rights = KB2_GPU_SPAN_RIGHT_READ,
                .record_schema_id = KB2_GPU_DRM_MODE_RECORD_OBJECT_ID,
                .element_count = wire.value.count_connectors,
                .flags = KB2_GPU_SPAN_FLAG_INPUT};
            plan->span_count = 1;
        }
        if (wire.value.mode_valid) {
            size_t index = plan->span_count++;
            plan->arguments[index + 1] = (kb2_gpu_argument_t){
                .argument_id = 3, .kind = KB2_GPU_ARGUMENT_SPAN,
                .flags = KB2_GPU_ARGUMENT_FLAG_INPUT,
                .record_schema_id = KB2_GPU_DRM_MODE_RECORD_MODE_INFO,
                .value = index + 1, .count = 1};
            plan->spans[index] = (kb2_gpu_span_t){.span_id = index + 1,
                .region_id = region_id, .offset = mode_offset,
                .length = sizeof(wire.value.mode),
                .rights = KB2_GPU_SPAN_RIGHT_READ,
                .record_schema_id = KB2_GPU_DRM_MODE_RECORD_MODE_INFO,
                .element_count = 1, .flags = KB2_GPU_SPAN_FLAG_INPUT};
        }
        return 0;
    }
    case GPUD_DRM_IOCTL_MODE_CURSOR:
    case GPUD_DRM_IOCTL_MODE_CURSOR2: {
        const int cursor2 = request->request == GPUD_DRM_IOCTL_MODE_CURSOR2;
        gpud_drm_mode_cursor2_t value = {0};
        const size_t size = cursor2 ? sizeof(value) : sizeof(value.cursor);
        if (request->arg_size != size || request->data_size != size)
            return -EINVAL;
        memcpy(&value, request->data, size);
        const gpud_drm_mode_cursor_t *cursor = &value.cursor;
        if (!cursor->crtc_id || !cursor->flags ||
            (cursor->flags & ~(GPUD_DRM_MODE_CURSOR_BO |
                              GPUD_DRM_MODE_CURSOR_MOVE)))
            return -EINVAL;
        plan->id = cursor2 ? KB2_GPU_DRM_MODE_COMMAND_CURSOR2 :
            KB2_GPU_DRM_MODE_COMMAND_CURSOR;
        plan->record = cursor2 ? KB2_GPU_DRM_MODE_RECORD_CURSOR2_REQUEST :
            KB2_GPU_DRM_MODE_RECORD_CURSOR_REQUEST;
        plan->size = cursor2 ? KB2_GPU_DRM_MODE_RECORD_CURSOR2_REQUEST_SIZE :
            KB2_GPU_DRM_MODE_RECORD_CURSOR_REQUEST_SIZE;
        write_u64(plan->data, 1);
        write_u32(plan->data + 8, cursor->flags);
        write_u32(plan->data + 12, cursor->crtc_id);
        write_u32(plan->data + 16, (uint32_t)cursor->x);
        write_u32(plan->data + 20, (uint32_t)cursor->y);
        write_u32(plan->data + 24, cursor->width);
        write_u32(plan->data + 28, cursor->height);
        write_u32(plan->data + 32, cursor->handle);
        if (cursor2) {
            write_u32(plan->data + 36, (uint32_t)value.hot_x);
            write_u32(plan->data + 40, (uint32_t)value.hot_y);
        }
        return 0;
    }
    case GPUD_DRM_IOCTL_MODE_PAGE_FLIP: {
        gpud_drm_mode_crtc_page_flip_t flip;
        if (request->arg_size != sizeof(flip) ||
            request->data_size != sizeof(flip))
            return -EINVAL;
        memcpy(&flip, request->data, sizeof(flip));
        const uint32_t target = flip.flags &
            (GPUD_DRM_MODE_PAGE_FLIP_TARGET_ABSOLUTE |
             GPUD_DRM_MODE_PAGE_FLIP_TARGET_RELATIVE);
        if (!flip.crtc_id || !flip.fb_id ||
            (flip.flags & ~GPUD_DRM_MODE_PAGE_FLIP_FLAGS) ||
            target == (GPUD_DRM_MODE_PAGE_FLIP_TARGET_ABSOLUTE |
                GPUD_DRM_MODE_PAGE_FLIP_TARGET_RELATIVE) ||
            (!target && flip.reserved) ||
            (!(flip.flags & GPUD_DRM_MODE_PAGE_FLIP_EVENT) &&
                flip.user_data))
            return -EINVAL;
        plan->id = KB2_GPU_DRM_MODE_COMMAND_PAGE_FLIP;
        plan->record = KB2_GPU_DRM_MODE_RECORD_PAGE_FLIP_REQUEST;
        plan->size = KB2_GPU_DRM_MODE_RECORD_PAGE_FLIP_REQUEST_SIZE;
        write_u64(plan->data, 1);
        write_u32(plan->data + 8, flip.crtc_id);
        write_u32(plan->data + 12, flip.fb_id);
        write_u32(plan->data + 16, flip.flags);
        write_u32(plan->data + 20, flip.reserved);
        write_u64(plan->data + 24, flip.user_data);
        return 0;
    }
    case GPUD_DRM_IOCTL_MODE_DIRTYFB: {
        gpud_drm_mode_fb_dirty_t dirty;
        if (request->arg_size != sizeof(dirty) ||
            request->data_size != sizeof(dirty))
            return -EINVAL;
        memcpy(&dirty, request->data, sizeof(dirty));
        const uint64_t rectangle_bytes =
            (uint64_t)dirty.num_clips * sizeof(gpud_drm_mode_rectangle_t);
        if ((dirty.flags & ~GPUD_DRM_MODE_DIRTY_FLAGS) ||
            dirty.num_clips > GPUD_DRM_MODE_DIRTY_MAX_CLIPS ||
            dirty.clips_ptr || request->aux_size != rectangle_bytes ||
            (dirty.num_clips && !region_id) ||
            ((dirty.flags & GPUD_DRM_MODE_DIRTY_ANNOTATE_COPY) &&
                (dirty.num_clips & 1u)))
            return -EINVAL;
        plan->id = KB2_GPU_DRM_MODE_COMMAND_DIRTY_FB;
        plan->record = KB2_GPU_DRM_MODE_RECORD_DIRTY_FB_REQUEST;
        plan->size = KB2_GPU_DRM_MODE_RECORD_DIRTY_FB_REQUEST_SIZE;
        write_u64(plan->data +
            KB2_GPU_DRM_MODE_RECORD_DIRTY_FB_REQUEST_TOPOLOGY_EPOCH_OFFSET, 1);
        write_u32(plan->data +
            KB2_GPU_DRM_MODE_RECORD_DIRTY_FB_REQUEST_FB_ID_OFFSET,
            dirty.fb_id);
        write_u32(plan->data +
            KB2_GPU_DRM_MODE_RECORD_DIRTY_FB_REQUEST_FLAGS_OFFSET,
            dirty.flags);
        write_u32(plan->data +
            KB2_GPU_DRM_MODE_RECORD_DIRTY_FB_REQUEST_COLOR_OFFSET,
            dirty.color);
        write_u32(plan->data +
            KB2_GPU_DRM_MODE_RECORD_DIRTY_FB_REQUEST_RECTANGLE_COUNT_OFFSET,
            dirty.num_clips);
        if (dirty.num_clips) {
            out->region = (kb2_gpu_region_t){
                .region_id = region_id,
                .rights = KB2_GPU_SPAN_RIGHT_READ,
                .length = rectangle_bytes,
            };
            plan->arguments[1] = (kb2_gpu_argument_t){
                .argument_id = 2,
                .kind = KB2_GPU_ARGUMENT_SPAN,
                .flags = KB2_GPU_ARGUMENT_FLAG_INPUT,
                .record_schema_id = KB2_GPU_DRM_MODE_RECORD_RECTANGLE,
                .value = 1,
                .count = dirty.num_clips,
            };
            plan->spans[0] = (kb2_gpu_span_t){
                .span_id = 1,
                .region_id = region_id,
                .length = rectangle_bytes,
                .rights = KB2_GPU_SPAN_RIGHT_READ,
                .record_schema_id = KB2_GPU_DRM_MODE_RECORD_RECTANGLE,
                .element_count = dirty.num_clips,
                .flags = KB2_GPU_SPAN_FLAG_INPUT,
            };
            plan->span_count = 1;
        }
        return 0;
    }
    default:
        return -EOPNOTSUPP;
    }
}

static int syncobj_array_plan(struct command_plan *plan,
    struct gpud_drm_translation *out, const gpud_drm_ioctl_request_t *request,
    uint32_t region_id) {
    uint32_t command;
    if (request->request == IOCTL_SYNCOBJ_WAIT)
        command = KB2_GPU_DRM_CORE_COMMAND_SYNCOBJ_WAIT;
    else if (request->request == IOCTL_SYNCOBJ_RESET)
        command = KB2_GPU_DRM_CORE_COMMAND_SYNCOBJ_RESET;
    else if (request->request == IOCTL_SYNCOBJ_SIGNAL)
        command = KB2_GPU_DRM_CORE_COMMAND_SYNCOBJ_SIGNAL;
    else
        return -EOPNOTSUPP;

    const int wait = command == KB2_GPU_DRM_CORE_COMMAND_SYNCOBJ_WAIT;
    const size_t abi_size = wait ? sizeof(gpud_drm_syncobj_wait_t) :
        sizeof(gpud_drm_syncobj_array_t);
    const uint32_t count = read_u32(request->data + (wait ? 16 : 8));
    const uint64_t handles_bytes = (uint64_t)count * sizeof(uint32_t);
    if (request->arg_size != abi_size || request->data_size != abi_size ||
        !region_id || !count || count > KB2_GPU_DRM_CORE_MAX_SYNCOBJS ||
        handles_bytes > SIZE_MAX || request->aux_size != handles_bytes ||
        read_u64(request->data) ||
        read_u32(request->data + (wait ? 28 : 12)))
        return -EINVAL;

    plan->set = KB2_GPU_DRM_CORE_SET_ID;
    plan->id = command;
    plan->record = wait ? KB2_GPU_DRM_CORE_RECORD_SYNCOBJ_WAIT_REQUEST :
        KB2_GPU_DRM_CORE_RECORD_SYNCOBJ_ARRAY_REQUEST;
    plan->size = wait ? KB2_GPU_DRM_CORE_RECORD_SYNCOBJ_WAIT_REQUEST_SIZE :
        KB2_GPU_DRM_CORE_RECORD_SYNCOBJ_ARRAY_REQUEST_SIZE;
    write_u32(plan->data, count);
    if (wait) {
        const uint32_t flags = read_u32(request->data + 20);
        const int64_t timeout = (int64_t)read_u64(request->data + 8);
        const uint64_t fence_deadline = read_u64(request->data + 32);
        if ((flags & ~15u) || (!(flags & 8u) && fence_deadline) || timeout < -1)
            return -EINVAL;
        write_u32(plan->data + 4, flags);
        write_u64(plan->data + 8, fence_deadline);
        /* The protocol requires an absolute nonzero deadline for wait commands.
         * Linux uses -1 for an unbounded wait and accepts past deadlines for a
         * nonblocking status query. */
        plan->deadline_ns = timeout == -1 ? UINT64_MAX :
            (timeout > 0 ? (uint64_t)timeout : UINT64_C(1));
    }
    plan->arguments[1] = (kb2_gpu_argument_t){
        .argument_id = 2, .kind = KB2_GPU_ARGUMENT_SPAN,
        .flags = KB2_GPU_ARGUMENT_FLAG_INPUT,
        .record_schema_id = KB2_GPU_DRM_CORE_RECORD_HANDLE_U32,
        .value = 1, .count = count};
    plan->spans[0] = (kb2_gpu_span_t){
        .span_id = 1, .region_id = region_id, .length = handles_bytes,
        .rights = KB2_GPU_SPAN_RIGHT_READ,
        .record_schema_id = KB2_GPU_DRM_CORE_RECORD_HANDLE_U32,
        .element_count = count, .flags = KB2_GPU_SPAN_FLAG_INPUT};
    plan->span_count = 1;
    out->region = (kb2_gpu_region_t){.region_id = region_id,
        .rights = KB2_GPU_SPAN_RIGHT_READ, .length = handles_bytes};
    return 0;
}

static int virtgpu_plan(
    struct command_plan *plan, struct gpud_drm_translation *out,
    const gpud_drm_ioctl_request_t *request, uint32_t region_id) {
    plan->set = KB2_GPU_DRM_VIRTGPU_SET_ID;
    switch (request->request) {
    case IOCTL_VIRTGPU_MAP:
        if (request->arg_size != sizeof(gpud_drm_virtgpu_map_t) ||
            request->data_size != sizeof(gpud_drm_virtgpu_map_t) ||
            read_u64(request->data) || !read_u32(request->data + 8) ||
            read_u32(request->data + 12))
            return -EINVAL;
        out->mapping_handle = read_u32(request->data + 8);
        plan->id = KB2_GPU_DRM_VIRTGPU_COMMAND_MAP;
        plan->record = KB2_GPU_DRM_VIRTGPU_RECORD_MAP_REQUEST;
        plan->size = KB2_GPU_DRM_VIRTGPU_RECORD_MAP_REQUEST_SIZE;
        write_u32(plan->data +
            KB2_GPU_DRM_VIRTGPU_RECORD_MAP_REQUEST_HANDLE_OFFSET,
            read_u32(request->data + 8));
        write_u32(plan->data +
            KB2_GPU_DRM_VIRTGPU_RECORD_MAP_REQUEST_MAPPING_RIGHTS_OFFSET,
            KB2_GPU_SPAN_RIGHT_READ | KB2_GPU_SPAN_RIGHT_WRITE);
        return 0;
    case IOCTL_VIRTGPU_GETPARAM:
        if (request->arg_size != 16 || request->data_size != 16)
            return -EINVAL;
        plan->id = KB2_GPU_DRM_VIRTGPU_COMMAND_GETPARAM;
        plan->record = KB2_GPU_DRM_VIRTGPU_RECORD_GETPARAM_REQUEST;
        plan->size = KB2_GPU_DRM_VIRTGPU_RECORD_GETPARAM_REQUEST_SIZE;
        write_u64(plan->data, read_u64(request->data));
        return 0;
    case IOCTL_VIRTGPU_EXECBUFFER: {
        uint32_t flags = read_u32(request->data);
        uint32_t command_bytes = read_u32(request->data + 4);
        uint32_t handle_count = read_u32(request->data + 24);
        uint32_t ring_index = read_u32(request->data + 32);
        uint32_t syncobj_stride = read_u32(request->data + 36);
        uint32_t input_count = read_u32(request->data + 40);
        uint32_t output_count = read_u32(request->data + 44);
        uint64_t command_extent = ((uint64_t)command_bytes + 7) & ~UINT64_C(7);
        uint64_t handle_bytes = (uint64_t)handle_count * sizeof(uint32_t);
        uint64_t syncobj_offset = (command_extent + handle_bytes + 7) & ~UINT64_C(7);
        uint64_t input_bytes = (uint64_t)input_count *
            KB2_GPU_DRM_VIRTGPU_RECORD_EXEC_SYNCOBJ_SIZE;
        uint64_t output_offset = syncobj_offset + input_bytes;
        uint64_t output_bytes = (uint64_t)output_count *
            KB2_GPU_DRM_VIRTGPU_RECORD_EXEC_SYNCOBJ_SIZE;
        uint64_t total_bytes = input_count || output_count ?
            output_offset + output_bytes : command_extent + handle_bytes;

        if (request->arg_size != sizeof(gpud_drm_virtgpu_execbuffer_t) ||
            request->data_size != sizeof(gpud_drm_virtgpu_execbuffer_t))
            return -EINVAL;
        if ((flags & ~(GPUD_DRM_VIRTGPU_EXECBUF_FENCE_FD_IN |
                GPUD_DRM_VIRTGPU_EXECBUF_FENCE_FD_OUT |
                GPUD_DRM_VIRTGPU_EXECBUF_RING_IDX)) ||
            !command_bytes ||
            command_bytes > KB2_GPU_DRM_VIRTGPU_MAX_COMMAND_BYTES ||
            handle_count > KB2_GPU_DRM_VIRTGPU_MAX_BO_HANDLES ||
            input_count > KB2_GPU_DRM_VIRTGPU_MAX_SYNCOBJS ||
            output_count > KB2_GPU_DRM_VIRTGPU_MAX_SYNCOBJS ||
            (!(flags & GPUD_DRM_VIRTGPU_EXECBUF_RING_IDX) && ring_index) ||
            ((input_count || output_count) && syncobj_stride !=
                KB2_GPU_DRM_VIRTGPU_RECORD_EXEC_SYNCOBJ_SIZE) ||
            (!(input_count || output_count) && syncobj_stride &&
                syncobj_stride != KB2_GPU_DRM_VIRTGPU_RECORD_EXEC_SYNCOBJ_SIZE) ||
            command_extent < command_bytes ||
            handle_bytes / sizeof(uint32_t) != handle_count ||
            syncobj_offset < command_extent + handle_bytes ||
            output_offset < syncobj_offset ||
            ((input_count || output_count) && total_bytes < output_offset) ||
            request->aux_size != total_bytes || read_u64(request->data + 8) ||
            read_u64(request->data + 16) != command_extent ||
            read_u32(request->data + 28) != UINT32_MAX ||
            read_u64(request->data + 48) !=
                (input_count ? syncobj_offset : 0) ||
            read_u64(request->data + 56) !=
                (output_count ? output_offset : 0) ||
            !region_id)
            return -EINVAL;
        plan->id = KB2_GPU_DRM_VIRTGPU_COMMAND_EXECBUFFER;
        plan->record = KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_REQUEST;
        plan->size = KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_REQUEST_SIZE;
        write_u32(plan->data +
            KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_REQUEST_COMMAND_BYTES_OFFSET,
            command_bytes);
        write_u32(plan->data +
            KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_REQUEST_BO_HANDLE_COUNT_OFFSET,
            handle_count);
        write_u32(plan->data +
            KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_REQUEST_FLAGS_OFFSET,
            flags & ~GPUD_DRM_VIRTGPU_EXECBUF_FENCE_FD_IN);
        write_u32(plan->data +
            KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_REQUEST_RING_INDEX_OFFSET,
            ring_index);
        write_u32(plan->data +
            KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_REQUEST_INPUT_SYNCOBJ_COUNT_OFFSET,
            input_count);
        write_u32(plan->data +
            KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_REQUEST_OUTPUT_SYNCOBJ_COUNT_OFFSET,
            output_count);
        plan->arguments[1] = (kb2_gpu_argument_t){
            .argument_id = 2, .kind = KB2_GPU_ARGUMENT_SPAN,
            .flags = KB2_GPU_ARGUMENT_FLAG_INPUT,
            .record_schema_id = KB2_GPU_DRM_VIRTGPU_RECORD_BYTE,
            .value = 1, .count = command_bytes};
        plan->spans[0] = (kb2_gpu_span_t){
            .span_id = 1, .region_id = region_id, .length = command_bytes,
            .rights = KB2_GPU_SPAN_RIGHT_READ,
            .record_schema_id = KB2_GPU_DRM_VIRTGPU_RECORD_BYTE,
            .element_count = command_bytes, .flags = KB2_GPU_SPAN_FLAG_INPUT};
        plan->span_count = 1;
        if (handle_count) {
            plan->arguments[2] = (kb2_gpu_argument_t){
                .argument_id = 3, .kind = KB2_GPU_ARGUMENT_SPAN,
                .flags = KB2_GPU_ARGUMENT_FLAG_INPUT,
                .record_schema_id = KB2_GPU_DRM_VIRTGPU_RECORD_HANDLE_U32,
                .value = 2, .count = handle_count};
            plan->spans[1] = (kb2_gpu_span_t){
                .span_id = 2, .region_id = region_id,
                .offset = command_extent, .length = handle_bytes,
                .rights = KB2_GPU_SPAN_RIGHT_READ,
                .record_schema_id = KB2_GPU_DRM_VIRTGPU_RECORD_HANDLE_U32,
                .element_count = handle_count, .flags = KB2_GPU_SPAN_FLAG_INPUT};
            plan->span_count = 2;
        }
        if (input_count) {
            size_t index = plan->span_count++;
            plan->arguments[index + 1] = (kb2_gpu_argument_t){
                .argument_id = 4, .kind = KB2_GPU_ARGUMENT_SPAN,
                .flags = KB2_GPU_ARGUMENT_FLAG_INPUT,
                .record_schema_id = KB2_GPU_DRM_VIRTGPU_RECORD_EXEC_SYNCOBJ,
                .value = index + 1, .count = input_count};
            plan->spans[index] = (kb2_gpu_span_t){
                .span_id = index + 1, .region_id = region_id,
                .offset = syncobj_offset, .length = input_bytes,
                .rights = KB2_GPU_SPAN_RIGHT_READ,
                .record_schema_id = KB2_GPU_DRM_VIRTGPU_RECORD_EXEC_SYNCOBJ,
                .element_count = input_count, .flags = KB2_GPU_SPAN_FLAG_INPUT};
        }
        if (output_count) {
            size_t index = plan->span_count++;
            plan->arguments[index + 1] = (kb2_gpu_argument_t){
                .argument_id = 5, .kind = KB2_GPU_ARGUMENT_SPAN,
                .flags = KB2_GPU_ARGUMENT_FLAG_INPUT,
                .record_schema_id = KB2_GPU_DRM_VIRTGPU_RECORD_EXEC_SYNCOBJ,
                .value = index + 1, .count = output_count};
            plan->spans[index] = (kb2_gpu_span_t){
                .span_id = index + 1, .region_id = region_id,
                .offset = output_offset, .length = output_bytes,
                .rights = KB2_GPU_SPAN_RIGHT_READ,
                .record_schema_id = KB2_GPU_DRM_VIRTGPU_RECORD_EXEC_SYNCOBJ,
                .element_count = output_count, .flags = KB2_GPU_SPAN_FLAG_INPUT};
        }
        out->region = (kb2_gpu_region_t){
            .region_id = region_id, .rights = KB2_GPU_SPAN_RIGHT_READ,
            .length = request->aux_size};
        return 0;
    }
    case IOCTL_VIRTGPU_RESOURCE_CREATE:
        if (request->arg_size != 56 || request->data_size != 56)
            return -EINVAL;
        plan->id = KB2_GPU_DRM_VIRTGPU_COMMAND_RESOURCE_CREATE;
        plan->record = KB2_GPU_DRM_VIRTGPU_RECORD_RESOURCE_CREATE_REQUEST;
        plan->size = KB2_GPU_DRM_VIRTGPU_RECORD_RESOURCE_CREATE_REQUEST_SIZE;
        memcpy(plan->data, request->data, 40);
        write_u32(plan->data + 40, read_u32(request->data + 40));
        write_u32(plan->data + 44, read_u32(request->data + 48));
        write_u32(plan->data + 48, read_u32(request->data + 52));
        return 0;
    case IOCTL_VIRTGPU_RESOURCE_INFO:
        if (request->arg_size != 16 || request->data_size != 16)
            return -EINVAL;
        plan->id = KB2_GPU_DRM_VIRTGPU_COMMAND_RESOURCE_INFO;
        plan->record = KB2_GPU_DRM_VIRTGPU_RECORD_RESOURCE_INFO_REQUEST;
        plan->size = KB2_GPU_DRM_VIRTGPU_RECORD_RESOURCE_INFO_REQUEST_SIZE;
        write_u32(plan->data, read_u32(request->data));
        return 0;
    case IOCTL_VIRTGPU_TRANSFER_FROM_HOST:
    case IOCTL_VIRTGPU_TRANSFER_TO_HOST:
        if (request->arg_size != 44 || request->data_size != 44)
            return -EINVAL;
        plan->id = request->request == IOCTL_VIRTGPU_TRANSFER_FROM_HOST ?
            KB2_GPU_DRM_VIRTGPU_COMMAND_TRANSFER_FROM_HOST_3D :
            KB2_GPU_DRM_VIRTGPU_COMMAND_TRANSFER_TO_HOST_3D;
        plan->record = KB2_GPU_DRM_VIRTGPU_RECORD_TRANSFER_REQUEST;
        plan->size = KB2_GPU_DRM_VIRTGPU_RECORD_TRANSFER_REQUEST_SIZE;
        memcpy(plan->data, request->data, plan->size);
        return 0;
    case IOCTL_VIRTGPU_WAIT:
        if (request->arg_size != sizeof(gpud_drm_virtgpu_3d_wait_t) ||
            request->data_size != sizeof(gpud_drm_virtgpu_3d_wait_t) ||
            !read_u32(request->data) ||
            (read_u32(request->data + 4) & ~GPUD_DRM_VIRTGPU_WAIT_NOWAIT))
            return -EINVAL;
        plan->id = KB2_GPU_DRM_VIRTGPU_COMMAND_WAIT;
        plan->record = KB2_GPU_DRM_VIRTGPU_RECORD_WAIT_REQUEST;
        plan->size = KB2_GPU_DRM_VIRTGPU_RECORD_WAIT_REQUEST_SIZE;
        memcpy(plan->data, request->data, plan->size);
        plan->deadline_ns = read_u32(request->data + 4) ? 1 : UINT64_MAX;
        return 0;
    case GPUD_DRM_IOCTL_VIRTGPU_GET_CAPS: {
        if (request->arg_size != sizeof(gpud_drm_virtgpu_get_caps_t) ||
            request->data_size != sizeof(gpud_drm_virtgpu_get_caps_t) ||
            !request->aux_size ||
            request->aux_size > KB2_GPU_DRM_VIRTGPU_MAX_CAPSET_BYTES ||
            read_u64(request->data + 8) ||
            read_u32(request->data + 16) != request->aux_size ||
            read_u32(request->data + 20) || !region_id)
            return -EINVAL;
        plan->id = KB2_GPU_DRM_VIRTGPU_COMMAND_GET_CAPS;
        plan->record = KB2_GPU_DRM_VIRTGPU_RECORD_GET_CAPS_REQUEST;
        plan->size = KB2_GPU_DRM_VIRTGPU_RECORD_GET_CAPS_REQUEST_SIZE;
        write_u32(plan->data, read_u32(request->data));
        write_u32(plan->data + 4, read_u32(request->data + 4));
        write_u32(plan->data + 8, request->aux_size);
        plan->arguments[1] = (kb2_gpu_argument_t){
            .argument_id = 2, .kind = KB2_GPU_ARGUMENT_SPAN,
            .flags = KB2_GPU_ARGUMENT_FLAG_OUTPUT,
            .record_schema_id = KB2_GPU_DRM_VIRTGPU_RECORD_BYTE,
            .value = 1, .count = request->aux_size};
        plan->spans[0] = (kb2_gpu_span_t){
            .span_id = 1, .region_id = region_id, .length = request->aux_size,
            .rights = KB2_GPU_SPAN_RIGHT_WRITE,
            .record_schema_id = KB2_GPU_DRM_VIRTGPU_RECORD_BYTE,
            .element_count = request->aux_size,
            .flags = KB2_GPU_SPAN_FLAG_OUTPUT};
        plan->span_count = 1;
        out->region = (kb2_gpu_region_t){
            .region_id = region_id, .rights = KB2_GPU_SPAN_RIGHT_WRITE,
            .length = request->aux_size};
        return 0;
    }
    case IOCTL_VIRTGPU_CONTEXT_INIT: {
        uint32_t mask = read_u32(request->data);
        uint32_t ring_count = read_u32(request->data + 8);
        uint32_t debug_bytes = read_u32(request->data + 12);
        uint64_t poll_mask = read_u64(request->data + 16);
        uint32_t known = GPUD_DRM_VIRTGPU_CONTEXT_HAS_CAPSET_ID |
            GPUD_DRM_VIRTGPU_CONTEXT_HAS_NUM_RINGS |
            GPUD_DRM_VIRTGPU_CONTEXT_HAS_POLL_RINGS_MASK |
            GPUD_DRM_VIRTGPU_CONTEXT_HAS_DEBUG_NAME;

        if (request->arg_size != sizeof(gpud_drm_virtgpu_context_init_t) ||
            request->data_size != sizeof(gpud_drm_virtgpu_context_wire_t) ||
            !mask || (mask & ~known) || read_u32(request->data + 4) > 63 ||
            ring_count > 64 ||
            (!(mask & GPUD_DRM_VIRTGPU_CONTEXT_HAS_CAPSET_ID) &&
             read_u32(request->data + 4)) ||
            (!(mask & GPUD_DRM_VIRTGPU_CONTEXT_HAS_NUM_RINGS) && ring_count) ||
            (!(mask & GPUD_DRM_VIRTGPU_CONTEXT_HAS_POLL_RINGS_MASK) && poll_mask) ||
            ((mask & GPUD_DRM_VIRTGPU_CONTEXT_HAS_POLL_RINGS_MASK) &&
             (!(mask & GPUD_DRM_VIRTGPU_CONTEXT_HAS_NUM_RINGS) || !ring_count ||
              (ring_count < 64 && (poll_mask >> ring_count)))) ||
            debug_bytes > KB2_GPU_DRM_VIRTGPU_MAX_DEBUG_NAME_BYTES ||
            (!!(mask & GPUD_DRM_VIRTGPU_CONTEXT_HAS_DEBUG_NAME) != !!debug_bytes) ||
            request->aux_size != debug_bytes || (debug_bytes && !region_id))
            return -EINVAL;
        plan->id = KB2_GPU_DRM_VIRTGPU_COMMAND_CONTEXT_INIT;
        plan->record = KB2_GPU_DRM_VIRTGPU_RECORD_CONTEXT_INIT_REQUEST;
        plan->size = KB2_GPU_DRM_VIRTGPU_RECORD_CONTEXT_INIT_REQUEST_SIZE;
        memcpy(plan->data, request->data, plan->size);
        if (debug_bytes) {
            plan->arguments[1] = (kb2_gpu_argument_t){
                .argument_id = 2, .kind = KB2_GPU_ARGUMENT_SPAN,
                .flags = KB2_GPU_ARGUMENT_FLAG_INPUT,
                .record_schema_id = KB2_GPU_DRM_VIRTGPU_RECORD_BYTE,
                .value = 1, .count = debug_bytes};
            plan->spans[0] = (kb2_gpu_span_t){
                .span_id = 1, .region_id = region_id, .length = debug_bytes,
                .rights = KB2_GPU_SPAN_RIGHT_READ,
                .record_schema_id = KB2_GPU_DRM_VIRTGPU_RECORD_BYTE,
                .element_count = debug_bytes, .flags = KB2_GPU_SPAN_FLAG_INPUT};
            plan->span_count = 1;
            out->region = (kb2_gpu_region_t){
                .region_id = region_id, .rights = KB2_GPU_SPAN_RIGHT_READ,
                .length = debug_bytes};
        }
        return 0;
    }
    default:
        return -EOPNOTSUPP;
    }
}

static int encode_plan(struct gpud_drm_translation *out,
    const struct gpud_drm_binding *binding,
    struct gpud_drm_translation *candidate, struct command_plan *plan) {
    const size_t inline_count = plan->record ? 1 : 0;
    if (inline_count)
        plan->arguments[0] = (kb2_gpu_argument_t){
            .argument_id = KB2_GPU_DRM_CORE_INLINE_ARGUMENT_ID,
            .kind = KB2_GPU_ARGUMENT_INLINE,
            .flags = KB2_GPU_ARGUMENT_FLAG_INPUT,
            .record_schema_id = plan->record, .count = plan->size};
    const uint32_t queue_class = plan->set == KB2_GPU_DRM_MODE_SET_ID ?
        KB2_GPU_QUEUE_DISPLAY : KB2_GPU_QUEUE_EXECUTION;
    kb2_gpu_command_source_t source = {.generation = binding->generation,
        .session_id = binding->session_id, .profile_kind = KB2_GPU_PROFILE_VIRGL,
        .queue_class = queue_class, .command_set_id = plan->set,
        .command_id = plan->id, .arguments = plan->arguments,
        .argument_count = inline_count + plan->span_count +
            plan->attachment_count,
        .spans = plan->spans, .span_count = plan->span_count,
        .attachments = plan->attachments,
        .attachment_count = plan->attachment_count,
        .inline_data = plan->data, .inline_length = plan->size,
        .deadline_ns = plan->deadline_ns,
        .regions = &candidate->region, .region_count = plan->span_count ? 1 : 0};
    kb2_protocol_status_t status = kb2_gpu_command_encode(candidate->command,
        sizeof(candidate->command), &candidate->command_size, &source);
    if (status != KB2_PROTOCOL_OK) return -EINVAL;
    candidate->command_set_id = plan->set;
    candidate->command_id = plan->id;
    candidate->queue_class = source.queue_class;
    *out = *candidate;
    return 0;
}

int gpud_drm_ioctl_encode(struct gpud_drm_translation *out,
    const struct gpud_drm_binding *binding, const gpud_drm_ioctl_request_t *request,
    uint32_t region_id) {
    if (!out || !binding || !request || !binding->generation || !binding->frontend_handle ||
        !binding->session_id || request->request > UINT32_MAX || request->reserved0)
        return -EINVAL;
    if (request->handle != binding->frontend_handle) return -EACCES;
    if (request->fd_flags & ~GPUD_DRM_IOCTL_FD_MASK)
        return -EINVAL;
    if (request->request == IOCTL_VIRTGPU_EXECBUFFER) {
        uint32_t flags = read_u32(request->data);
        if (!!(flags & GPUD_DRM_VIRTGPU_EXECBUF_FENCE_FD_IN) !=
                !!(request->fd_flags & GPUD_DRM_IOCTL_FD_INPUT_WAIT) ||
            !!(flags & GPUD_DRM_VIRTGPU_EXECBUF_FENCE_FD_OUT) !=
                !!(request->fd_flags & GPUD_DRM_IOCTL_FD_OUTPUT_NOTIFY))
            return -EINVAL;
    } else if (request->fd_flags) {
        return -EOPNOTSUPP;
    }
    if (request->aux_size &&
        request->request != GPUD_DRM_IOCTL_VIRTGPU_GET_CAPS &&
        request->request != IOCTL_VIRTGPU_EXECBUFFER &&
        request->request != IOCTL_VIRTGPU_CONTEXT_INIT &&
        request->request != GPUD_DRM_IOCTL_MODE_DIRTYFB &&
        request->request != IOCTL_SYNCOBJ_WAIT &&
        request->request != IOCTL_SYNCOBJ_RESET &&
        request->request != IOCTL_SYNCOBJ_SIGNAL)
        return -EOPNOTSUPP;
    struct gpud_drm_translation candidate = {.generation = binding->generation,
        .frontend_handle = binding->frontend_handle, .session_id = binding->session_id};
    struct command_plan plan = {0};
    int result;
    if (request->request == IOCTL_VERSION) {
        result = version_plan(&plan, &candidate, request, region_id);
    } else {
        result = scalar_plan(&plan, request);
        if (result == -EOPNOTSUPP)
            result = syncobj_array_plan(&plan, &candidate, request, region_id);
        if (result == -EOPNOTSUPP)
            result = virtgpu_plan(&plan, &candidate, request, region_id);
        if (result == -EOPNOTSUPP)
            result = mode_plan(&plan, &candidate, request, region_id);
    }
    return result ? result : encode_plan(out, binding, &candidate, &plan);
}

int gpud_drm_prime_export_encode(struct gpud_drm_translation *out,
    const struct gpud_drm_binding *binding, uint32_t handle, uint32_t flags) {
    if (!out || !binding || !binding->generation || !binding->frontend_handle ||
        !binding->session_id || !handle ||
        (flags & ~(GPUD_DRM_CLOEXEC | GPUD_DRM_RDWR)))
        return -EINVAL;
    struct gpud_drm_translation candidate = {.generation = binding->generation,
        .frontend_handle = binding->frontend_handle, .session_id = binding->session_id};
    struct command_plan plan = {.set = KB2_GPU_DRM_CORE_SET_ID,
        .id = KB2_GPU_DRM_CORE_COMMAND_PRIME_HANDLE_TO_ATTACHMENT,
        .record = KB2_GPU_DRM_CORE_RECORD_PRIME_EXPORT_REQUEST,
        .size = KB2_GPU_DRM_CORE_RECORD_PRIME_EXPORT_REQUEST_SIZE};
    write_u32(plan.data, handle);
    write_u32(plan.data + 4, flags);
    return encode_plan(out, binding, &candidate, &plan);
}

int gpud_drm_prime_import_encode(struct gpud_drm_translation *out,
    const struct gpud_drm_binding *binding, uint64_t token) {
    if (!out || !binding || !binding->generation || !binding->frontend_handle ||
        !binding->session_id || !token)
        return -EINVAL;
    struct gpud_drm_translation candidate = {.generation = binding->generation,
        .frontend_handle = binding->frontend_handle, .session_id = binding->session_id};
    struct command_plan plan = {.set = KB2_GPU_DRM_CORE_SET_ID,
        .id = KB2_GPU_DRM_CORE_COMMAND_PRIME_ATTACHMENT_TO_HANDLE,
        .record = KB2_GPU_DRM_CORE_RECORD_PRIME_IMPORT_REQUEST,
        .size = KB2_GPU_DRM_CORE_RECORD_PRIME_IMPORT_REQUEST_SIZE,
        .attachment_count = 1};
    plan.arguments[1] = (kb2_gpu_argument_t){.argument_id = 2,
        .kind = KB2_GPU_ARGUMENT_ATTACHMENT,
        .flags = KB2_GPU_ARGUMENT_FLAG_INPUT, .value = 1, .count = 1};
    plan.attachments[0] = (kb2_gpu_attachment_t){.attachment_id = 1,
        .object_class = KB2_GPU_ATTACHMENT_DMA_BUF, .exchange_id = token,
        .generation = binding->generation,
        .rights = KB2_GPU_SPAN_RIGHT_READ | KB2_GPU_SPAN_RIGHT_WRITE,
        .role = 1, .ownership = KB2_GPU_ATTACHMENT_SHARE,
        .flags = KB2_GPU_ATTACHMENT_FLAG_INPUT};
    return encode_plan(out, binding, &candidate, &plan);
}

int gpud_drm_poll_encode(struct gpud_drm_translation *out,
    const struct gpud_drm_binding *binding, uint32_t events) {
    if (!out || !binding || !binding->generation || !binding->frontend_handle ||
        !binding->session_id || (events & ~KB2_GPU_DRM_CORE_POLL_READABLE))
        return -EINVAL;
    struct gpud_drm_translation candidate = {.generation = binding->generation,
        .frontend_handle = binding->frontend_handle, .session_id = binding->session_id};
    struct command_plan plan = {.set = KB2_GPU_DRM_CORE_SET_ID,
        .id = KB2_GPU_DRM_CORE_COMMAND_POLL_EVENTS,
        .record = KB2_GPU_DRM_CORE_RECORD_SCALAR_U32,
        .size = KB2_GPU_DRM_CORE_RECORD_SCALAR_U32_SIZE};
    write_u32(plan.data, events);
    return encode_plan(out, binding, &candidate, &plan);
}

int gpud_drm_read_encode(struct gpud_drm_translation *out,
    const struct gpud_drm_binding *binding, uint32_t capacity,
    uint32_t region_id) {
    if (!out || !binding || !binding->generation || !binding->frontend_handle ||
        !binding->session_id || !capacity ||
        capacity > KB2_GPU_DRM_CORE_MAX_EVENT_BYTES || !region_id)
        return -EINVAL;
    struct gpud_drm_translation candidate = {.generation = binding->generation,
        .frontend_handle = binding->frontend_handle, .session_id = binding->session_id,
        .region = {.region_id = region_id, .rights = KB2_GPU_SPAN_RIGHT_WRITE,
            .length = capacity}};
    struct command_plan plan = {.set = KB2_GPU_DRM_CORE_SET_ID,
        .id = KB2_GPU_DRM_CORE_COMMAND_READ_EVENTS,
        .record = KB2_GPU_DRM_CORE_RECORD_LENGTH_REQUEST,
        .size = KB2_GPU_DRM_CORE_RECORD_LENGTH_REQUEST_SIZE,
        .span_count = 1};
    write_u32(plan.data, capacity);
    plan.arguments[1] = (kb2_gpu_argument_t){.argument_id = 2,
        .kind = KB2_GPU_ARGUMENT_SPAN, .flags = KB2_GPU_ARGUMENT_FLAG_OUTPUT,
        .record_schema_id = KB2_GPU_DRM_CORE_RECORD_BYTE, .value = 1,
        .count = capacity};
    plan.spans[0] = (kb2_gpu_span_t){.span_id = 1, .region_id = region_id,
        .length = capacity, .rights = KB2_GPU_SPAN_RIGHT_WRITE,
        .record_schema_id = KB2_GPU_DRM_CORE_RECORD_BYTE,
        .element_count = capacity, .flags = KB2_GPU_SPAN_FLAG_OUTPUT};
    return encode_plan(out, binding, &candidate, &plan);
}
