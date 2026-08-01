#pragma once

#include <stdatomic.h>
#include <stdint.h>

#include <kobox/module.h>

typedef void (*drmd_virtio_gpu_unref_callback_fn)(void *, void *);
typedef int (*drmd_virtio_gpu_queue_fenced_fn)(void *, void *, void *);

typedef struct drmd_virtio_gpu_unref_bridge {
    void *vgdev;
    drmd_virtio_gpu_queue_fenced_fn queue_fenced;
    drmd_virtio_gpu_unref_callback_fn upstream_unref_callback;
} drmd_virtio_gpu_unref_bridge_t;

typedef struct drmd_virtio_gpu_unref_request {
    _Atomic uint32_t callback_done;
    _Atomic uint64_t expected_fence_id;
    uint32_t submitted;
    uint32_t response_type;
    uint32_t response_flags;
    uint64_t response_fence_id;
    void *object;
    void *fence;
    drmd_virtio_gpu_unref_callback_fn upstream_unref_callback;
} drmd_virtio_gpu_unref_request_t;

typedef struct drmd_virtio_gpu_unref_result {
    uint32_t response_type;
    uint32_t response_flags;
    uint64_t response_fence_id;
} drmd_virtio_gpu_unref_result_t;

int drmd_virtio_gpu_unref_bridge_init(
    drmd_virtio_gpu_unref_bridge_t *bridge,
    kb_module_t *module,
    void *vgdev);

/* The caller must enter the virtio-gpu module context before submission. */
int drmd_virtio_gpu_unref_request_submit(
    const drmd_virtio_gpu_unref_bridge_t *bridge,
    drmd_virtio_gpu_unref_request_t *request,
    void *object,
    void *fence);

/* Returns zero while pending, one on an exact successful response, or -errno. */
int drmd_virtio_gpu_unref_request_poll(
    const drmd_virtio_gpu_unref_request_t *request,
    drmd_virtio_gpu_unref_result_t *out_result);
