#pragma once

#include <stddef.h>
#include <stdint.h>

enum {
    DRMD_SYNCOBJ_OBJECT_CAPACITY = 128,
    DRMD_SYNCOBJ_POINT_CAPACITY = 512,
    DRMD_SYNCOBJ_FENCE_CAPACITY = 96,
    DRMD_SYNCOBJ_EXPORT_CAPACITY = 96,
};

typedef struct drmd_syncobj_fd_ops {
    /* Return 1 when the wait is complete, 0 while pending, or a negative errno. */
    int (*poll)(void *context, int fd);
    int (*signal)(void *context, int fd);
    void (*close)(void *context, int fd);
} drmd_syncobj_fd_ops_t;

typedef struct drmd_syncobj_object {
    int active;
    uint64_t owner;
    uint32_t handle;
    int signaled_valid;
    uint64_t signaled_point;
} drmd_syncobj_object_t;

typedef struct drmd_syncobj_fence {
    int active;
    int wait_fd;
    uint32_t refs;
} drmd_syncobj_fence_t;

typedef struct drmd_syncobj_point {
    int active;
    uint16_t object_slot;
    uint16_t fence_slot;
    uint64_t point;
} drmd_syncobj_point_t;

typedef struct drmd_syncobj_export {
    int active;
    uint16_t fence_slot;
    int notify_fd;
    uint64_t owner;
} drmd_syncobj_export_t;

typedef struct drmd_syncobj_state {
    drmd_syncobj_fd_ops_t ops;
    void *ops_context;
    uint32_t next_handle;
    drmd_syncobj_object_t objects[DRMD_SYNCOBJ_OBJECT_CAPACITY];
    drmd_syncobj_point_t points[DRMD_SYNCOBJ_POINT_CAPACITY];
    drmd_syncobj_fence_t fences[DRMD_SYNCOBJ_FENCE_CAPACITY];
    drmd_syncobj_export_t exports[DRMD_SYNCOBJ_EXPORT_CAPACITY];
} drmd_syncobj_state_t;

void drmd_syncobj_state_init(
    drmd_syncobj_state_t *state,
    const drmd_syncobj_fd_ops_t *ops,
    void *ops_context);
void drmd_syncobj_state_finish(drmd_syncobj_state_t *state);
int drmd_syncobj_create(
    drmd_syncobj_state_t *state,
    uint64_t owner,
    uint32_t flags,
    uint32_t *out_handle);
int drmd_syncobj_destroy(
    drmd_syncobj_state_t *state,
    uint64_t owner,
    uint32_t handle);
int drmd_syncobj_import_sync_file(
    drmd_syncobj_state_t *state,
    uint64_t owner,
    uint32_t handle,
    uint32_t flags,
    int wait_fd);
int drmd_syncobj_transfer(
    drmd_syncobj_state_t *state,
    uint64_t owner,
    uint32_t src_handle,
    uint64_t src_point,
    uint32_t dst_handle,
    uint64_t dst_point,
    uint32_t flags);
int drmd_syncobj_export_sync_file(
    drmd_syncobj_state_t *state,
    uint64_t owner,
    uint32_t handle,
    uint32_t flags,
    int notify_fd);
void drmd_syncobj_owner_close(drmd_syncobj_state_t *state, uint64_t owner);
void drmd_syncobj_progress(drmd_syncobj_state_t *state);
size_t drmd_syncobj_collect_wait_fds(
    const drmd_syncobj_state_t *state,
    int *out_fds,
    size_t capacity);
size_t drmd_syncobj_active_object_count(const drmd_syncobj_state_t *state);
size_t drmd_syncobj_active_point_count(const drmd_syncobj_state_t *state);
size_t drmd_syncobj_active_fence_count(const drmd_syncobj_state_t *state);
size_t drmd_syncobj_active_export_count(const drmd_syncobj_state_t *state);
