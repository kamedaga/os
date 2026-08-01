#pragma once

#include "drmd/kms_abi.h"

#include <stdint.h>

typedef struct drmd_atomic_snapshot {
    uint32_t connector_crtc_id;
    uint32_t crtc_active;
    uint32_t crtc_mode_id;
    uint32_t plane_crtc_id;
    uint32_t plane_fb_id;
    uint64_t src_x;
    uint64_t src_y;
    uint64_t src_w;
    uint64_t src_h;
    int64_t crtc_x;
    int64_t crtc_y;
    uint64_t crtc_w;
    uint64_t crtc_h;
} drmd_atomic_snapshot_t;

typedef struct drmd_atomic_stage_result {
    drmd_atomic_snapshot_t snapshot;
    int modeset_changed;
    int saw_input_fence;
} drmd_atomic_stage_result_t;

typedef struct drmd_atomic_resource_info {
    int mode_exists;
    uint32_t mode_width;
    uint32_t mode_height;
    int fb_exists;
    uint32_t fb_width;
    uint32_t fb_height;
} drmd_atomic_resource_info_t;

typedef struct drmd_atomic_lifecycle {
    drmd_atomic_snapshot_t presented;
    drmd_atomic_snapshot_t pending;
    int pending_active;
} drmd_atomic_lifecycle_t;

int drmd_atomic_stage(
    const drmd_atomic_snapshot_t *current,
    const drmd_mode_atomic_wire_t *wire,
    int has_input_wait,
    drmd_atomic_stage_result_t *out);
int drmd_atomic_validate_resources(
    const drmd_atomic_snapshot_t *snapshot,
    const drmd_atomic_resource_info_t *resources);
void drmd_atomic_lifecycle_init(drmd_atomic_lifecycle_t *lifecycle);
int drmd_atomic_lifecycle_begin(
    drmd_atomic_lifecycle_t *lifecycle,
    const drmd_atomic_snapshot_t *pending,
    int test_only);
int drmd_atomic_lifecycle_complete(drmd_atomic_lifecycle_t *lifecycle);
void drmd_atomic_lifecycle_cancel(drmd_atomic_lifecycle_t *lifecycle);
