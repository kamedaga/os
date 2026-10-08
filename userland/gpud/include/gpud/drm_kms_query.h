#pragma once

#include <stddef.h>
#include <gpud/drm_kms_abi.h>

/* Inline vectors are an IPC optimization, not a display topology limit.
 * Keep their Linux scalar layout separate so a count/retry can use the
 * existing typed auxiliary region without forwarding user pointers. */
struct gpud_kms_vector {
    size_t count, pointer, second_pointer, inline_array, second_array;
    uint32_t limit, stride;
    int partial;
};
struct gpud_kms_query_shape {
    size_t scalar_size, inline_size;
    unsigned int vectors;
    struct gpud_kms_vector vector[4];
};

static inline int gpud_kms_query_shape(uint32_t command,
    struct gpud_kms_query_shape *shape)
{
    *shape = (struct gpud_kms_query_shape){0};
#define VECTOR(T, W, count_field, ptr_field, array_field, cap, bytes, part) \
    (struct gpud_kms_vector){.count = offsetof(T, count_field), \
        .pointer = offsetof(T, ptr_field), .inline_array = offsetof(W, array_field), \
        .limit = cap, .stride = bytes, .partial = part}
    switch (command) {
    case GPUD_DRM_IOCTL_MODE_GETRESOURCES:
        shape->scalar_size = sizeof(gpud_drm_mode_card_res_t);
        shape->inline_size = sizeof(gpud_drm_kms_resources_wire_t);
        shape->vectors = 4;
        shape->vector[0] = VECTOR(gpud_drm_mode_card_res_t, gpud_drm_kms_resources_wire_t,
            count_fbs, fb_id_ptr, fbs, GPUD_DRM_KMS_FB_CAPACITY, 4, 0);
        shape->vector[1] = VECTOR(gpud_drm_mode_card_res_t, gpud_drm_kms_resources_wire_t,
            count_crtcs, crtc_id_ptr, crtcs, GPUD_DRM_KMS_CRTC_CAPACITY, 4, 0);
        shape->vector[2] = VECTOR(gpud_drm_mode_card_res_t, gpud_drm_kms_resources_wire_t,
            count_connectors, connector_id_ptr, connectors, GPUD_DRM_KMS_CONNECTOR_CAPACITY, 4, 0);
        shape->vector[3] = VECTOR(gpud_drm_mode_card_res_t, gpud_drm_kms_resources_wire_t,
            count_encoders, encoder_id_ptr, encoders, GPUD_DRM_KMS_ENCODER_CAPACITY, 4, 0);
        return 1;
    case GPUD_DRM_IOCTL_MODE_GETPLANERESOURCES:
        shape->scalar_size = sizeof(gpud_drm_mode_get_plane_res_t);
        shape->inline_size = sizeof(gpud_drm_kms_plane_res_wire_t);
        shape->vectors = 1;
        shape->vector[0] = VECTOR(gpud_drm_mode_get_plane_res_t, gpud_drm_kms_plane_res_wire_t,
            count_planes, plane_id_ptr, planes, GPUD_DRM_KMS_PLANE_CAPACITY, 4, 1);
        return 1;
    case GPUD_DRM_IOCTL_MODE_GETPLANE:
        shape->scalar_size = sizeof(gpud_drm_mode_get_plane_t);
        shape->inline_size = sizeof(gpud_drm_kms_plane_wire_t);
        shape->vectors = 1;
        shape->vector[0] = VECTOR(gpud_drm_mode_get_plane_t, gpud_drm_kms_plane_wire_t,
            count_format_types, format_type_ptr, formats, GPUD_DRM_KMS_FORMAT_CAPACITY, 4, 0);
        return 1;
    case GPUD_DRM_IOCTL_MODE_GETPROPERTY:
        shape->scalar_size = sizeof(gpud_drm_mode_get_property_t);
        shape->inline_size = sizeof(gpud_drm_kms_property_wire_t);
        shape->vectors = 2;
        shape->vector[0] = VECTOR(gpud_drm_mode_get_property_t, gpud_drm_kms_property_wire_t,
            count_values, values_ptr, values, GPUD_DRM_KMS_PROPERTY_VALUE_CAPACITY, 8, 1);
        shape->vector[1] = VECTOR(gpud_drm_mode_get_property_t, gpud_drm_kms_property_wire_t,
            count_enum_blobs, enum_blob_ptr, enums, GPUD_DRM_KMS_PROPERTY_ENUM_CAPACITY, 40, 1);
        return 1;
    case GPUD_DRM_IOCTL_MODE_OBJ_GETPROPERTIES:
        shape->scalar_size = sizeof(gpud_drm_mode_obj_get_properties_t);
        shape->inline_size = sizeof(gpud_drm_kms_object_properties_wire_t);
        shape->vectors = 1;
        shape->vector[0] = VECTOR(gpud_drm_mode_obj_get_properties_t,
            gpud_drm_kms_object_properties_wire_t, count_props, props_ptr, props,
            GPUD_DRM_KMS_PROPERTY_CAPACITY, 16, 1);
        shape->vector[0].second_pointer = offsetof(gpud_drm_mode_obj_get_properties_t, prop_values_ptr);
        shape->vector[0].second_array = offsetof(gpud_drm_kms_object_properties_wire_t, prop_values);
        return 1;
    default:
        return 0;
    }
#undef VECTOR
}
