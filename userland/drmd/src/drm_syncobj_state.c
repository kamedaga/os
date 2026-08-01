#include "drm_syncobj_state.h"

#include <string.h>

enum {
    DRMD_EBADF = 9,
    DRMD_EINVAL = 22,
    DRMD_EMFILE = 24,
    DRMD_ENOENT = 2,
};

static drmd_syncobj_object_t *find_object(
    drmd_syncobj_state_t *state,
    uint64_t owner,
    uint32_t handle,
    size_t *out_slot)
{
    if (state == NULL || handle == 0) return NULL;
    for (size_t i = 0; i < DRMD_SYNCOBJ_OBJECT_CAPACITY; i++) {
        drmd_syncobj_object_t *object = &state->objects[i];
        if (object->active && object->owner == owner && object->handle == handle) {
            if (out_slot != NULL) *out_slot = i;
            return object;
        }
    }
    return NULL;
}

static void fence_unref(drmd_syncobj_state_t *state, size_t slot)
{
    if (state == NULL || slot >= DRMD_SYNCOBJ_FENCE_CAPACITY) return;
    drmd_syncobj_fence_t *fence = &state->fences[slot];
    if (!fence->active || fence->refs == 0) return;
    fence->refs--;
    if (fence->refs != 0) return;
    if (fence->wait_fd >= 0 && state->ops.close != NULL) {
        state->ops.close(state->ops_context, fence->wait_fd);
    }
    memset(fence, 0, sizeof(*fence));
    fence->wait_fd = -1;
}

static void remove_point(drmd_syncobj_state_t *state, size_t slot)
{
    if (state == NULL || slot >= DRMD_SYNCOBJ_POINT_CAPACITY ||
        !state->points[slot].active) return;
    const size_t fence_slot = state->points[slot].fence_slot;
    memset(&state->points[slot], 0, sizeof(state->points[slot]));
    fence_unref(state, fence_slot);
}

static void remove_object_points(drmd_syncobj_state_t *state, size_t object_slot)
{
    for (size_t i = 0; i < DRMD_SYNCOBJ_POINT_CAPACITY; i++) {
        if (state->points[i].active && state->points[i].object_slot == object_slot) {
            remove_point(state, i);
        }
    }
}

static int object_point_is_signaled(
    const drmd_syncobj_object_t *object,
    uint64_t point)
{
    return object != NULL && object->signaled_valid &&
        point <= object->signaled_point;
}

static drmd_syncobj_point_t *find_point(
    drmd_syncobj_state_t *state,
    size_t object_slot,
    uint64_t point,
    size_t *out_slot)
{
    for (size_t i = 0; i < DRMD_SYNCOBJ_POINT_CAPACITY; i++) {
        drmd_syncobj_point_t *entry = &state->points[i];
        if (entry->active && entry->object_slot == object_slot &&
            entry->point == point) {
            if (out_slot != NULL) *out_slot = i;
            return entry;
        }
    }
    return NULL;
}

static int alloc_point(
    drmd_syncobj_state_t *state,
    size_t object_slot,
    uint64_t point,
    size_t fence_slot)
{
    size_t old_slot = 0;
    if (find_point(state, object_slot, point, &old_slot) != NULL) {
        remove_point(state, old_slot);
    }
    for (size_t i = 0; i < DRMD_SYNCOBJ_POINT_CAPACITY; i++) {
        if (state->points[i].active) continue;
        state->points[i].active = 1;
        state->points[i].object_slot = (uint16_t)object_slot;
        state->points[i].fence_slot = (uint16_t)fence_slot;
        state->points[i].point = point;
        state->fences[fence_slot].refs++;
        return 0;
    }
    return -DRMD_EMFILE;
}

static void signal_object_point(
    drmd_syncobj_state_t *state,
    size_t object_slot,
    uint64_t point)
{
    drmd_syncobj_object_t *object = &state->objects[object_slot];
    if (!object->signaled_valid || point > object->signaled_point) {
        object->signaled_valid = 1;
        object->signaled_point = point;
    }
    for (size_t i = 0; i < DRMD_SYNCOBJ_POINT_CAPACITY; i++) {
        if (state->points[i].active &&
            state->points[i].object_slot == object_slot &&
            state->points[i].point <= object->signaled_point) {
            remove_point(state, i);
        }
    }
}

static int fence_ready(drmd_syncobj_state_t *state, size_t fence_slot)
{
    drmd_syncobj_fence_t *fence = &state->fences[fence_slot];
    if (!fence->active || fence->wait_fd < 0 || state->ops.poll == NULL) return 0;
    return state->ops.poll(state->ops_context, fence->wait_fd);
}

void drmd_syncobj_state_init(
    drmd_syncobj_state_t *state,
    const drmd_syncobj_fd_ops_t *ops,
    void *ops_context)
{
    if (state == NULL) return;
    memset(state, 0, sizeof(*state));
    if (ops != NULL) state->ops = *ops;
    state->ops_context = ops_context;
    state->next_handle = 1;
    for (size_t i = 0; i < DRMD_SYNCOBJ_FENCE_CAPACITY; i++) {
        state->fences[i].wait_fd = -1;
    }
    for (size_t i = 0; i < DRMD_SYNCOBJ_EXPORT_CAPACITY; i++) {
        state->exports[i].notify_fd = -1;
    }
}

void drmd_syncobj_state_finish(drmd_syncobj_state_t *state)
{
    if (state == NULL) return;
    for (size_t i = 0; i < DRMD_SYNCOBJ_EXPORT_CAPACITY; i++) {
        if (state->exports[i].active && state->exports[i].notify_fd >= 0 &&
            state->ops.close != NULL) {
            state->ops.close(state->ops_context, state->exports[i].notify_fd);
        }
    }
    for (size_t i = 0; i < DRMD_SYNCOBJ_FENCE_CAPACITY; i++) {
        if (state->fences[i].active && state->fences[i].wait_fd >= 0 &&
            state->ops.close != NULL) {
            state->ops.close(state->ops_context, state->fences[i].wait_fd);
        }
    }
    drmd_syncobj_fd_ops_t ops = state->ops;
    void *context = state->ops_context;
    memset(state, 0, sizeof(*state));
    state->ops = ops;
    state->ops_context = context;
}

int drmd_syncobj_create(
    drmd_syncobj_state_t *state,
    uint64_t owner,
    uint32_t flags,
    uint32_t *out_handle)
{
    if (state == NULL || out_handle == NULL || flags != 0) return -DRMD_EINVAL;
    for (size_t i = 0; i < DRMD_SYNCOBJ_OBJECT_CAPACITY; i++) {
        if (state->objects[i].active) continue;
        uint32_t handle = state->next_handle++;
        if (handle == 0) handle = state->next_handle++;
        while (find_object(state, owner, handle, NULL) != NULL) {
            handle = state->next_handle++;
            if (handle == 0) handle = state->next_handle++;
        }
        memset(&state->objects[i], 0, sizeof(state->objects[i]));
        state->objects[i].active = 1;
        state->objects[i].owner = owner;
        state->objects[i].handle = handle;
        *out_handle = handle;
        return 0;
    }
    return -DRMD_EMFILE;
}

int drmd_syncobj_destroy(
    drmd_syncobj_state_t *state,
    uint64_t owner,
    uint32_t handle)
{
    size_t slot = 0;
    drmd_syncobj_object_t *object = find_object(state, owner, handle, &slot);
    if (object == NULL) return -DRMD_ENOENT;
    remove_object_points(state, slot);
    memset(object, 0, sizeof(*object));
    return 0;
}

int drmd_syncobj_import_sync_file(
    drmd_syncobj_state_t *state,
    uint64_t owner,
    uint32_t handle,
    uint32_t flags,
    int wait_fd)
{
    size_t object_slot = 0;
    drmd_syncobj_object_t *object = find_object(state, owner, handle, &object_slot);
    if (object == NULL) return -DRMD_ENOENT;
    if (flags != 1u || wait_fd < 0) return -DRMD_EBADF;

    int ready = state->ops.poll != NULL ?
        state->ops.poll(state->ops_context, wait_fd) : 0;
    if (ready < 0) return ready;
    if (ready != 0) {
        if (state->ops.close != NULL) state->ops.close(state->ops_context, wait_fd);
        signal_object_point(state, object_slot, 0);
        return 0;
    }

    size_t fence_slot = DRMD_SYNCOBJ_FENCE_CAPACITY;
    for (size_t i = 0; i < DRMD_SYNCOBJ_FENCE_CAPACITY; i++) {
        if (!state->fences[i].active) {
            fence_slot = i;
            break;
        }
    }
    if (fence_slot == DRMD_SYNCOBJ_FENCE_CAPACITY) return -DRMD_EMFILE;
    size_t point_slot = 0;
    drmd_syncobj_point_t *old = find_point(state, object_slot, 0, &point_slot);
    const int had_old = old != NULL;
    if (!had_old) {
        size_t free_points = 0;
        for (size_t i = 0; i < DRMD_SYNCOBJ_POINT_CAPACITY; i++) {
            if (!state->points[i].active) free_points++;
        }
        if (free_points == 0) return -DRMD_EMFILE;
    }
    if (had_old) remove_point(state, point_slot);
    object->signaled_valid = 0;
    object->signaled_point = 0;
    drmd_syncobj_fence_t *fence = &state->fences[fence_slot];
    memset(fence, 0, sizeof(*fence));
    fence->active = 1;
    fence->wait_fd = wait_fd;
    const int status = alloc_point(state, object_slot, 0, fence_slot);
    if (status != 0) {
        memset(fence, 0, sizeof(*fence));
        fence->wait_fd = -1;
        return status;
    }
    return 0;
}

int drmd_syncobj_transfer(
    drmd_syncobj_state_t *state,
    uint64_t owner,
    uint32_t src_handle,
    uint64_t src_point,
    uint32_t dst_handle,
    uint64_t dst_point,
    uint32_t flags)
{
    size_t src_slot = 0;
    size_t dst_slot = 0;
    drmd_syncobj_object_t *src = find_object(state, owner, src_handle, &src_slot);
    drmd_syncobj_object_t *dst = find_object(state, owner, dst_handle, &dst_slot);
    if (src == NULL || dst == NULL) return -DRMD_ENOENT;
    if (flags != 0) return -DRMD_EINVAL;
    drmd_syncobj_progress(state);
    if (src_slot == dst_slot && src_point == dst_point) return 0;
    if (object_point_is_signaled(dst, dst_point)) return 0;
    if (object_point_is_signaled(src, src_point)) {
        signal_object_point(state, dst_slot, dst_point);
        return 0;
    }
    drmd_syncobj_point_t *source = find_point(state, src_slot, src_point, NULL);
    if (source == NULL || source->fence_slot >= DRMD_SYNCOBJ_FENCE_CAPACITY ||
        !state->fences[source->fence_slot].active) return -DRMD_EINVAL;
    if (find_point(state, dst_slot, dst_point, NULL) == NULL) {
        int have_free = 0;
        for (size_t i = 0; i < DRMD_SYNCOBJ_POINT_CAPACITY; i++) {
            if (!state->points[i].active) {
                have_free = 1;
                break;
            }
        }
        if (!have_free) return -DRMD_EMFILE;
    }
    return alloc_point(state, dst_slot, dst_point, source->fence_slot);
}

int drmd_syncobj_export_sync_file(
    drmd_syncobj_state_t *state,
    uint64_t owner,
    uint32_t handle,
    uint32_t flags,
    int notify_fd)
{
    size_t object_slot = 0;
    drmd_syncobj_object_t *object = find_object(state, owner, handle, &object_slot);
    if (object == NULL) return -DRMD_ENOENT;
    if (flags != 1u || notify_fd < 0) return -DRMD_EBADF;
    drmd_syncobj_progress(state);
    if (object_point_is_signaled(object, 0)) {
        const int status = state->ops.signal != NULL ?
            state->ops.signal(state->ops_context, notify_fd) : -DRMD_EINVAL;
        if (status != 0) return status;
        if (state->ops.close != NULL) state->ops.close(state->ops_context, notify_fd);
        return 0;
    }
    drmd_syncobj_point_t *point = find_point(state, object_slot, 0, NULL);
    if (point == NULL || !state->fences[point->fence_slot].active) {
        return -DRMD_EINVAL;
    }
    for (size_t i = 0; i < DRMD_SYNCOBJ_EXPORT_CAPACITY; i++) {
        if (state->exports[i].active) continue;
        state->exports[i].active = 1;
        state->exports[i].fence_slot = point->fence_slot;
        state->exports[i].notify_fd = notify_fd;
        state->exports[i].owner = owner;
        state->fences[point->fence_slot].refs++;
        return 0;
    }
    return -DRMD_EMFILE;
}

void drmd_syncobj_owner_close(drmd_syncobj_state_t *state, uint64_t owner)
{
    if (state == NULL) return;
    for (size_t i = 0; i < DRMD_SYNCOBJ_EXPORT_CAPACITY; i++) {
        drmd_syncobj_export_t *exported = &state->exports[i];
        if (!exported->active || exported->owner != owner) continue;
        if (exported->notify_fd >= 0 && state->ops.close != NULL) {
            state->ops.close(state->ops_context, exported->notify_fd);
        }
        const size_t fence_slot = exported->fence_slot;
        memset(exported, 0, sizeof(*exported));
        exported->notify_fd = -1;
        fence_unref(state, fence_slot);
    }
    for (size_t i = 0; i < DRMD_SYNCOBJ_OBJECT_CAPACITY; i++) {
        if (state->objects[i].active && state->objects[i].owner == owner) {
            const uint32_t handle = state->objects[i].handle;
            (void)drmd_syncobj_destroy(state, owner, handle);
        }
    }
}

void drmd_syncobj_progress(drmd_syncobj_state_t *state)
{
    if (state == NULL) return;
    uint8_t completed[DRMD_SYNCOBJ_FENCE_CAPACITY];
    uint8_t failed[DRMD_SYNCOBJ_FENCE_CAPACITY];
    memset(completed, 0, sizeof(completed));
    memset(failed, 0, sizeof(failed));
    for (size_t i = 0; i < DRMD_SYNCOBJ_FENCE_CAPACITY; i++) {
        if (!state->fences[i].active) continue;
        const int ready = fence_ready(state, i);
        if (ready > 0) completed[i] = 1;
        if (ready < 0) failed[i] = 1;
    }
    for (size_t i = 0; i < DRMD_SYNCOBJ_POINT_CAPACITY; i++) {
        drmd_syncobj_point_t *point = &state->points[i];
        if (!point->active || !completed[point->fence_slot]) continue;
        signal_object_point(state, point->object_slot, point->point);
    }
    for (size_t i = 0; i < DRMD_SYNCOBJ_POINT_CAPACITY; i++) {
        if (state->points[i].active && failed[state->points[i].fence_slot]) {
            remove_point(state, i);
        }
    }
    for (size_t i = 0; i < DRMD_SYNCOBJ_EXPORT_CAPACITY; i++) {
        drmd_syncobj_export_t *exported = &state->exports[i];
        if (!exported->active ||
            (!completed[exported->fence_slot] &&
                !failed[exported->fence_slot])) continue;
        if (completed[exported->fence_slot] && state->ops.signal != NULL) {
            (void)state->ops.signal(state->ops_context, exported->notify_fd);
        }
        if (state->ops.close != NULL) {
            state->ops.close(state->ops_context, exported->notify_fd);
        }
        const size_t fence_slot = exported->fence_slot;
        memset(exported, 0, sizeof(*exported));
        exported->notify_fd = -1;
        fence_unref(state, fence_slot);
    }
}

size_t drmd_syncobj_collect_wait_fds(
    const drmd_syncobj_state_t *state,
    int *out_fds,
    size_t capacity)
{
    if (state == NULL || out_fds == NULL) return 0;
    size_t count = 0;
    for (size_t i = 0; i < DRMD_SYNCOBJ_FENCE_CAPACITY && count < capacity; i++) {
        if (state->fences[i].active && state->fences[i].wait_fd >= 0) {
            out_fds[count++] = state->fences[i].wait_fd;
        }
    }
    return count;
}

#define DRMD_COUNT_ACTIVE(name, member, capacity) \
    size_t name(const drmd_syncobj_state_t *state) \
    { \
        size_t count = 0; \
        if (state == NULL) return 0; \
        for (size_t i = 0; i < (capacity); i++) count += state->member[i].active != 0; \
        return count; \
    }

DRMD_COUNT_ACTIVE(drmd_syncobj_active_object_count, objects, DRMD_SYNCOBJ_OBJECT_CAPACITY)
DRMD_COUNT_ACTIVE(drmd_syncobj_active_point_count, points, DRMD_SYNCOBJ_POINT_CAPACITY)
DRMD_COUNT_ACTIVE(drmd_syncobj_active_fence_count, fences, DRMD_SYNCOBJ_FENCE_CAPACITY)
DRMD_COUNT_ACTIVE(drmd_syncobj_active_export_count, exports, DRMD_SYNCOBJ_EXPORT_CAPACITY)

#undef DRMD_COUNT_ACTIVE
