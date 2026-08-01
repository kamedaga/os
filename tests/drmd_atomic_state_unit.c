#include "drm_atomic_state.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void add_property(
    drmd_mode_atomic_wire_t *wire,
    uint32_t *at,
    uint32_t property,
    uint64_t value)
{
    wire->props[*at] = property;
    wire->prop_values[*at] = value;
    (*at)++;
}

static drmd_mode_atomic_wire_t initial_modeset(void)
{
    drmd_mode_atomic_wire_t wire;
    memset(&wire, 0, sizeof(wire));
    wire.flags = DRMD_MODE_ATOMIC_ALLOW_MODESET;
    wire.count_objs = 3;
    wire.objects[0] = DRMD_KMS_CONNECTOR_ID;
    wire.objects[1] = DRMD_KMS_CRTC_ID;
    wire.objects[2] = DRMD_KMS_PLANE_ID;
    wire.object_prop_counts[0] = 1;
    wire.object_prop_counts[1] = 2;
    wire.object_prop_counts[2] = 11;
    uint32_t at = 0;
    add_property(&wire, &at, DRMD_KMS_CONNECTOR_CRTC_ID_PROP_ID, DRMD_KMS_CRTC_ID);
    add_property(&wire, &at, DRMD_KMS_CRTC_ACTIVE_PROP_ID, 1);
    add_property(&wire, &at, DRMD_KMS_CRTC_MODE_ID_PROP_ID, 202);
    add_property(&wire, &at, DRMD_KMS_PLANE_SRC_X_PROP_ID, 0);
    add_property(&wire, &at, DRMD_KMS_PLANE_SRC_Y_PROP_ID, 0);
    add_property(&wire, &at, DRMD_KMS_PLANE_SRC_W_PROP_ID, UINT64_C(1024) << 16);
    add_property(&wire, &at, DRMD_KMS_PLANE_SRC_H_PROP_ID, UINT64_C(768) << 16);
    add_property(&wire, &at, DRMD_KMS_PLANE_CRTC_X_PROP_ID, 0);
    add_property(&wire, &at, DRMD_KMS_PLANE_CRTC_Y_PROP_ID, 0);
    add_property(&wire, &at, DRMD_KMS_PLANE_CRTC_W_PROP_ID, 1024);
    add_property(&wire, &at, DRMD_KMS_PLANE_CRTC_H_PROP_ID, 768);
    add_property(&wire, &at, DRMD_KMS_PLANE_FB_ID_PROP_ID, 71);
    add_property(&wire, &at, DRMD_KMS_PLANE_CRTC_ID_PROP_ID, DRMD_KMS_CRTC_ID);
    add_property(&wire, &at, DRMD_KMS_PLANE_IN_FENCE_FD_PROP_ID, UINT64_MAX);
    wire.total_props = at;
    return wire;
}

int main(void)
{
    drmd_atomic_lifecycle_t lifecycle;
    drmd_atomic_lifecycle_init(&lifecycle);
    drmd_mode_atomic_wire_t wire = initial_modeset();
    drmd_atomic_stage_result_t staged;
    assert(drmd_atomic_stage(&lifecycle.presented, &wire, 0, &staged) == 0);
    assert(staged.modeset_changed);
    const drmd_atomic_resource_info_t resources = {
        .mode_exists = 1,
        .mode_width = 1024,
        .mode_height = 768,
        .fb_exists = 1,
        .fb_width = 1024,
        .fb_height = 768,
    };
    assert(drmd_atomic_validate_resources(&staged.snapshot, &resources) == 0);

    assert(drmd_atomic_lifecycle_begin(&lifecycle, &staged.snapshot, 1) == 0);
    assert(!lifecycle.pending_active);
    assert(lifecycle.presented.crtc_active == 0);

    assert(drmd_atomic_lifecycle_begin(&lifecycle, &staged.snapshot, 0) == 0);
    assert(lifecycle.pending_active);
    assert(drmd_atomic_lifecycle_begin(&lifecycle, &staged.snapshot, 0) == -16);
    assert(drmd_atomic_lifecycle_complete(&lifecycle) == 0);
    assert(lifecycle.presented.crtc_active == 1);
    assert(lifecycle.presented.plane_fb_id == 71);

    drmd_mode_atomic_wire_t flip;
    memset(&flip, 0, sizeof(flip));
    flip.flags = DRMD_MODE_ATOMIC_NONBLOCK | DRMD_MODE_PAGE_FLIP_EVENT;
    flip.count_objs = 1;
    flip.objects[0] = DRMD_KMS_PLANE_ID;
    flip.object_prop_counts[0] = 2;
    flip.total_props = 2;
    flip.props[0] = DRMD_KMS_PLANE_FB_ID_PROP_ID;
    flip.prop_values[0] = 72;
    flip.props[1] = DRMD_KMS_PLANE_IN_FENCE_FD_PROP_ID;
    flip.prop_values[1] = 0;
    assert(drmd_atomic_stage(&lifecycle.presented, &flip, 1, &staged) == 0);
    assert(!staged.modeset_changed);
    assert(staged.saw_input_fence);
    assert(drmd_atomic_lifecycle_begin(&lifecycle, &staged.snapshot, 0) == 0);
    drmd_atomic_lifecycle_cancel(&lifecycle);
    assert(!lifecycle.pending_active);
    assert(lifecycle.presented.plane_fb_id == 71);

    drmd_mode_atomic_wire_t invalid = flip;
    invalid.props[1] = DRMD_KMS_PLANE_FB_ID_PROP_ID;
    const drmd_atomic_snapshot_t before = lifecycle.presented;
    assert(drmd_atomic_stage(&lifecycle.presented, &invalid, 1, &staged) == -22);
    assert(memcmp(&before, &lifecycle.presented, sizeof(before)) == 0);

    wire = initial_modeset();
    wire.flags = 0;
    const drmd_atomic_snapshot_t disabled = {0};
    assert(drmd_atomic_stage(&disabled, &wire, 0, &staged) == -22);

    puts("drmd atomic state unit: ok");
    return 0;
}
