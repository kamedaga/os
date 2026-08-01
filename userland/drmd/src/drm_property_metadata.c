#include "drm_property_metadata.h"

#include "drmd/kms_abi.h"

#include <limits.h>
#include <string.h>

static void set_name(drmd_property_metadata_t *metadata, const char *name)
{
    strncpy(metadata->name, name, sizeof(metadata->name) - 1);
}

static void set_range(
    drmd_property_metadata_t *metadata,
    uint32_t flags,
    const char *name,
    uint64_t minimum,
    uint64_t maximum)
{
    metadata->flags = flags;
    metadata->count_values = 2;
    metadata->values[0] = minimum;
    metadata->values[1] = maximum;
    set_name(metadata, name);
}

static void set_object(
    drmd_property_metadata_t *metadata,
    const char *name,
    uint32_t object_type)
{
    metadata->flags = DRMD_MODE_PROP_OBJECT;
    metadata->count_values = 1;
    metadata->values[0] = object_type;
    set_name(metadata, name);
}

int drmd_atomic_property_metadata(
    uint32_t property_id,
    drmd_property_metadata_t *out)
{
    if (out == NULL) return -22;
    memset(out, 0, sizeof(*out));
    switch (property_id) {
    case DRMD_KMS_CONNECTOR_CRTC_ID_PROP_ID:
    case DRMD_KMS_PLANE_CRTC_ID_PROP_ID:
        set_object(out, "CRTC_ID", DRMD_MODE_OBJECT_CRTC);
        return 0;
    case DRMD_KMS_CRTC_ACTIVE_PROP_ID:
        set_range(out, DRMD_MODE_PROP_RANGE, "ACTIVE", 0, 1);
        return 0;
    case DRMD_KMS_CRTC_MODE_ID_PROP_ID:
        out->flags = DRMD_MODE_PROP_BLOB;
        set_name(out, "MODE_ID");
        return 0;
    case DRMD_KMS_PLANE_SRC_X_PROP_ID:
        set_range(out, DRMD_MODE_PROP_RANGE, "SRC_X", 0, UINT32_MAX);
        return 0;
    case DRMD_KMS_PLANE_SRC_Y_PROP_ID:
        set_range(out, DRMD_MODE_PROP_RANGE, "SRC_Y", 0, UINT32_MAX);
        return 0;
    case DRMD_KMS_PLANE_SRC_W_PROP_ID:
        set_range(out, DRMD_MODE_PROP_RANGE, "SRC_W", 0, UINT32_MAX);
        return 0;
    case DRMD_KMS_PLANE_SRC_H_PROP_ID:
        set_range(out, DRMD_MODE_PROP_RANGE, "SRC_H", 0, UINT32_MAX);
        return 0;
    case DRMD_KMS_PLANE_CRTC_X_PROP_ID:
        set_range(out, DRMD_MODE_PROP_SIGNED_RANGE, "CRTC_X",
            (uint64_t)(int64_t)INT32_MIN, INT32_MAX);
        return 0;
    case DRMD_KMS_PLANE_CRTC_Y_PROP_ID:
        set_range(out, DRMD_MODE_PROP_SIGNED_RANGE, "CRTC_Y",
            (uint64_t)(int64_t)INT32_MIN, INT32_MAX);
        return 0;
    case DRMD_KMS_PLANE_CRTC_W_PROP_ID:
        set_range(out, DRMD_MODE_PROP_RANGE, "CRTC_W", 0, UINT32_MAX);
        return 0;
    case DRMD_KMS_PLANE_CRTC_H_PROP_ID:
        set_range(out, DRMD_MODE_PROP_RANGE, "CRTC_H", 0, UINT32_MAX);
        return 0;
    case DRMD_KMS_PLANE_FB_ID_PROP_ID:
        set_object(out, "FB_ID", DRMD_MODE_OBJECT_FB);
        return 0;
    case DRMD_KMS_PLANE_IN_FENCE_FD_PROP_ID:
        set_range(out, DRMD_MODE_PROP_SIGNED_RANGE, "IN_FENCE_FD",
            UINT64_MAX, INT32_MAX);
        return 0;
    default:
        return -2;
    }
}
