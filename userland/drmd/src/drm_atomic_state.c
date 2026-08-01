#include "drm_atomic_state.h"

#include <limits.h>
#include <string.h>

enum {
    DRMD_EBUSY = 16,
    DRMD_EINVAL = 22,
};

static int checked_u32(uint64_t value, uint32_t *out)
{
    if (value > UINT32_MAX || out == NULL) return -DRMD_EINVAL;
    *out = (uint32_t)value;
    return 0;
}

static int checked_i32(uint64_t value, int64_t *out)
{
    const int64_t signed_value = (int64_t)value;
    if (signed_value < INT32_MIN || signed_value > INT32_MAX || out == NULL) {
        return -DRMD_EINVAL;
    }
    *out = signed_value;
    return 0;
}

static int set_connector_property(
    drmd_atomic_snapshot_t *snapshot,
    uint32_t prop,
    uint64_t value)
{
    if (prop != DRMD_KMS_CONNECTOR_CRTC_ID_PROP_ID ||
        (value != 0 && value != DRMD_KMS_CRTC_ID)) return -DRMD_EINVAL;
    snapshot->connector_crtc_id = (uint32_t)value;
    return 0;
}

static int set_crtc_property(
    drmd_atomic_snapshot_t *snapshot,
    uint32_t prop,
    uint64_t value)
{
    switch (prop) {
    case DRMD_KMS_CRTC_ACTIVE_PROP_ID:
        if (value > 1) return -DRMD_EINVAL;
        snapshot->crtc_active = (uint32_t)value;
        return 0;
    case DRMD_KMS_CRTC_MODE_ID_PROP_ID:
        return checked_u32(value, &snapshot->crtc_mode_id);
    default:
        return -DRMD_EINVAL;
    }
}

static int set_plane_property(
    drmd_atomic_snapshot_t *snapshot,
    uint32_t prop,
    uint64_t value,
    int has_input_wait,
    int *saw_input_fence)
{
    switch (prop) {
    case DRMD_KMS_PLANE_SRC_X_PROP_ID: snapshot->src_x = value; return 0;
    case DRMD_KMS_PLANE_SRC_Y_PROP_ID: snapshot->src_y = value; return 0;
    case DRMD_KMS_PLANE_SRC_W_PROP_ID: snapshot->src_w = value; return 0;
    case DRMD_KMS_PLANE_SRC_H_PROP_ID: snapshot->src_h = value; return 0;
    case DRMD_KMS_PLANE_CRTC_X_PROP_ID: return checked_i32(value, &snapshot->crtc_x);
    case DRMD_KMS_PLANE_CRTC_Y_PROP_ID: return checked_i32(value, &snapshot->crtc_y);
    case DRMD_KMS_PLANE_CRTC_W_PROP_ID: snapshot->crtc_w = value; return 0;
    case DRMD_KMS_PLANE_CRTC_H_PROP_ID: snapshot->crtc_h = value; return 0;
    case DRMD_KMS_PLANE_FB_ID_PROP_ID:
        return checked_u32(value, &snapshot->plane_fb_id);
    case DRMD_KMS_PLANE_CRTC_ID_PROP_ID:
        if (value != 0 && value != DRMD_KMS_CRTC_ID) return -DRMD_EINVAL;
        snapshot->plane_crtc_id = (uint32_t)value;
        return 0;
    case DRMD_KMS_PLANE_IN_FENCE_FD_PROP_ID:
        if (saw_input_fence == NULL || *saw_input_fence) return -DRMD_EINVAL;
        *saw_input_fence = 1;
        if (has_input_wait) return value == 0 ? 0 : -DRMD_EINVAL;
        return value == UINT64_MAX ? 0 : -DRMD_EINVAL;
    default:
        return -DRMD_EINVAL;
    }
}

static int property_is_duplicate(
    const drmd_mode_atomic_wire_t *wire,
    uint32_t first,
    uint32_t count,
    uint32_t at)
{
    for (uint32_t i = first; i < at && i < first + count; i++) {
        if (wire->props[i] == wire->props[at]) return 1;
    }
    return 0;
}

int drmd_atomic_stage(
    const drmd_atomic_snapshot_t *current,
    const drmd_mode_atomic_wire_t *wire,
    int has_input_wait,
    drmd_atomic_stage_result_t *out)
{
    if (current == NULL || wire == NULL || out == NULL ||
        wire->reserved0 != 0 || wire->count_objs > DRMD_ATOMIC_OBJECT_CAPACITY ||
        wire->total_props > DRMD_ATOMIC_PROPERTY_CAPACITY ||
        (wire->flags & ~DRMD_MODE_ATOMIC_FLAGS) != 0) return -DRMD_EINVAL;

    drmd_atomic_stage_result_t staged;
    memset(&staged, 0, sizeof(staged));
    staged.snapshot = *current;
    uint32_t property_index = 0;
    for (uint32_t object_index = 0; object_index < wire->count_objs; object_index++) {
        const uint32_t object = wire->objects[object_index];
        const uint32_t count = wire->object_prop_counts[object_index];
        if (count > wire->total_props - property_index) return -DRMD_EINVAL;
        for (uint32_t previous = 0; previous < object_index; previous++) {
            if (wire->objects[previous] == object) return -DRMD_EINVAL;
        }
        for (uint32_t i = 0; i < count; i++) {
            const uint32_t at = property_index + i;
            if (property_is_duplicate(wire, property_index, count, at)) {
                return -DRMD_EINVAL;
            }
            int status;
            if (object == DRMD_KMS_CONNECTOR_ID) {
                status = set_connector_property(
                    &staged.snapshot, wire->props[at], wire->prop_values[at]);
            } else if (object == DRMD_KMS_CRTC_ID) {
                status = set_crtc_property(
                    &staged.snapshot, wire->props[at], wire->prop_values[at]);
            } else if (object == DRMD_KMS_PLANE_ID) {
                status = set_plane_property(
                    &staged.snapshot,
                    wire->props[at],
                    wire->prop_values[at],
                    has_input_wait,
                    &staged.saw_input_fence);
            } else {
                return -DRMD_EINVAL;
            }
            if (status != 0) return status;
        }
        property_index += count;
    }
    if (property_index != wire->total_props ||
        (has_input_wait && !staged.saw_input_fence)) return -DRMD_EINVAL;

    staged.modeset_changed =
        staged.snapshot.connector_crtc_id != current->connector_crtc_id ||
        staged.snapshot.crtc_active != current->crtc_active ||
        staged.snapshot.crtc_mode_id != current->crtc_mode_id ||
        staged.snapshot.plane_crtc_id != current->plane_crtc_id;
    if (staged.modeset_changed &&
        (wire->flags & DRMD_MODE_ATOMIC_ALLOW_MODESET) == 0) return -DRMD_EINVAL;
    *out = staged;
    return 0;
}

int drmd_atomic_validate_resources(
    const drmd_atomic_snapshot_t *snapshot,
    const drmd_atomic_resource_info_t *resources)
{
    if (snapshot == NULL || resources == NULL) return -DRMD_EINVAL;
    const int disabled = snapshot->connector_crtc_id == 0 &&
        snapshot->crtc_active == 0 && snapshot->crtc_mode_id == 0 &&
        snapshot->plane_crtc_id == 0 && snapshot->plane_fb_id == 0;
    if (disabled) return 0;
    if (snapshot->connector_crtc_id != DRMD_KMS_CRTC_ID ||
        snapshot->crtc_active != 1 || snapshot->crtc_mode_id == 0 ||
        snapshot->plane_crtc_id != DRMD_KMS_CRTC_ID ||
        snapshot->plane_fb_id == 0 || !resources->mode_exists ||
        !resources->fb_exists || resources->mode_width == 0 ||
        resources->mode_height == 0 ||
        resources->fb_width != resources->mode_width ||
        resources->fb_height != resources->mode_height ||
        snapshot->src_x != 0 || snapshot->src_y != 0 ||
        snapshot->crtc_x != 0 || snapshot->crtc_y != 0 ||
        snapshot->src_w != ((uint64_t)resources->mode_width << 16) ||
        snapshot->src_h != ((uint64_t)resources->mode_height << 16) ||
        snapshot->crtc_w != resources->mode_width ||
        snapshot->crtc_h != resources->mode_height) return -DRMD_EINVAL;
    return 0;
}

void drmd_atomic_lifecycle_init(drmd_atomic_lifecycle_t *lifecycle)
{
    if (lifecycle != NULL) memset(lifecycle, 0, sizeof(*lifecycle));
}

int drmd_atomic_lifecycle_begin(
    drmd_atomic_lifecycle_t *lifecycle,
    const drmd_atomic_snapshot_t *pending,
    int test_only)
{
    if (lifecycle == NULL || pending == NULL) return -DRMD_EINVAL;
    if (lifecycle->pending_active) return -DRMD_EBUSY;
    if (test_only) return 0;
    lifecycle->pending = *pending;
    lifecycle->pending_active = 1;
    return 0;
}

int drmd_atomic_lifecycle_complete(drmd_atomic_lifecycle_t *lifecycle)
{
    if (lifecycle == NULL || !lifecycle->pending_active) return -DRMD_EINVAL;
    lifecycle->presented = lifecycle->pending;
    memset(&lifecycle->pending, 0, sizeof(lifecycle->pending));
    lifecycle->pending_active = 0;
    return 0;
}

void drmd_atomic_lifecycle_cancel(drmd_atomic_lifecycle_t *lifecycle)
{
    if (lifecycle == NULL) return;
    memset(&lifecycle->pending, 0, sizeof(lifecycle->pending));
    lifecycle->pending_active = 0;
}
