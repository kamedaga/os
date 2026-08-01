#include "virtio_gpu_unref_bridge.h"

#include <kobox/shim.h>

#include <stddef.h>
#include <string.h>

enum {
    DRMD_VIRTIO_GPU_VBUFFER_BYTES = 0x60,
    DRMD_VIRTIO_GPU_VBUFFER_CACHE_BYTES = 0xd8,
    DRMD_VIRTIO_GPU_VBUFS_OFFSET = 0xf238,
    DRMD_VIRTIO_GPU_OBJECT_RESOURCE_ID_OFFSET = 0x198,
    /* Linux 6.8 split BTF: struct virtio_gpu_fence, bit offset 576. */
    DRMD_VIRTIO_GPU_FENCE_ID_OFFSET = 0x48,
    DRMD_VIRTIO_GPU_CMD_RESOURCE_UNREF = 0x0102,
    DRMD_VIRTIO_GPU_RESP_OK_NODATA = 0x1100,
    DRMD_VIRTIO_GPU_FLAG_FENCE = 1u,
    DRMD_VIRTIO_GPU_UNREF_COMMAND_BYTES = 0x20,
    DRMD_VIRTIO_GPU_RESPONSE_BYTES = 0x18,
    DRMD_LINUX_GFP_ZERO = 0x100,
};

typedef struct drmd_virtio_gpu_list {
    void *next;
    void *previous;
} drmd_virtio_gpu_list_t;

typedef struct drmd_virtio_gpu_vbuffer {
    char *buffer;
    int size;
    void *data_buffer;
    uint32_t data_size;
    char *response_buffer;
    int response_size;
    drmd_virtio_gpu_unref_callback_fn response_callback;
    void *response_callback_data;
    void *objects;
    drmd_virtio_gpu_list_t list;
    uint32_t sequence;
} drmd_virtio_gpu_vbuffer_t;

typedef struct drmd_virtio_gpu_control_header {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t context_id;
    uint8_t ring_index;
    uint8_t padding[3];
} drmd_virtio_gpu_control_header_t;

typedef struct drmd_virtio_gpu_resource_unref {
    drmd_virtio_gpu_control_header_t header;
    uint32_t resource_id;
    uint32_t padding;
} drmd_virtio_gpu_resource_unref_t;

_Static_assert(sizeof(drmd_virtio_gpu_vbuffer_t) == DRMD_VIRTIO_GPU_VBUFFER_BYTES,
    "Linux 6.8 virtio_gpu_vbuffer size");
_Static_assert(offsetof(drmd_virtio_gpu_vbuffer_t, buffer) == 0x00,
    "Linux 6.8 virtio_gpu_vbuffer buffer offset");
_Static_assert(offsetof(drmd_virtio_gpu_vbuffer_t, size) == 0x08,
    "Linux 6.8 virtio_gpu_vbuffer size offset");
_Static_assert(offsetof(drmd_virtio_gpu_vbuffer_t, data_buffer) == 0x10,
    "Linux 6.8 virtio_gpu_vbuffer data buffer offset");
_Static_assert(offsetof(drmd_virtio_gpu_vbuffer_t, data_size) == 0x18,
    "Linux 6.8 virtio_gpu_vbuffer data size offset");
_Static_assert(offsetof(drmd_virtio_gpu_vbuffer_t, response_buffer) == 0x20,
    "Linux 6.8 virtio_gpu_vbuffer response buffer offset");
_Static_assert(offsetof(drmd_virtio_gpu_vbuffer_t, response_size) == 0x28,
    "Linux 6.8 virtio_gpu_vbuffer response size offset");
_Static_assert(offsetof(drmd_virtio_gpu_vbuffer_t, response_callback) == 0x30,
    "Linux 6.8 virtio_gpu_vbuffer response callback offset");
_Static_assert(offsetof(drmd_virtio_gpu_vbuffer_t, response_callback_data) == 0x38,
    "Linux 6.8 virtio_gpu_vbuffer callback data offset");
_Static_assert(offsetof(drmd_virtio_gpu_vbuffer_t, objects) == 0x40,
    "Linux 6.8 virtio_gpu_vbuffer objects offset");
_Static_assert(offsetof(drmd_virtio_gpu_vbuffer_t, list) == 0x48,
    "Linux 6.8 virtio_gpu_vbuffer list offset");
_Static_assert(offsetof(drmd_virtio_gpu_vbuffer_t, sequence) == 0x58,
    "Linux 6.8 virtio_gpu_vbuffer sequence offset");
_Static_assert(sizeof(drmd_virtio_gpu_control_header_t) == DRMD_VIRTIO_GPU_RESPONSE_BYTES,
    "virtio-gpu control header size");
_Static_assert(sizeof(drmd_virtio_gpu_resource_unref_t) == DRMD_VIRTIO_GPU_UNREF_COMMAND_BYTES,
    "virtio-gpu RESOURCE_UNREF command size");

static int has_bytes(const void *base, size_t offset, const void *expected, size_t size)
{
    return base != NULL && expected != NULL &&
        memcmp((const uint8_t *)base + offset, expected, size) == 0;
}

static int validate_linux_6_8_layout(
    const void *alloc_vbufs,
    const void *unref_resource,
    const void *unref_callback,
    const void *fence_emit)
{
    static const uint8_t alloc_slot[] = {0xbe, 0xd8, 0x00, 0x00, 0x00};
    static const uint8_t alloc_store[] = {0x48, 0x89, 0x83, 0x38, 0xf2, 0x00, 0x00};
    static const uint8_t cache_load[] = {0x48, 0x8b, 0xbf, 0x38, 0xf2, 0x00, 0x00};
    static const uint8_t command_base[] = {0x48, 0x83, 0xc0, 0x60};
    static const uint8_t response_base[] = {0x48, 0x8d, 0x86, 0x80, 0x00, 0x00, 0x00};
    static const uint8_t command_size[] = {0xc7, 0x46, 0x08, 0x20, 0x00, 0x00, 0x00};
    static const uint8_t response_size[] = {0xc7, 0x46, 0x28, 0x18, 0x00, 0x00, 0x00};
    static const uint8_t resource_load[] = {0x8b, 0x83, 0x98, 0x01, 0x00, 0x00};
    static const uint8_t command_type[] = {0xc7, 0x46, 0x60, 0x02, 0x01, 0x00, 0x00};
    static const uint8_t callback_data[] = {0x48, 0x89, 0x5e, 0x38};
    static const uint8_t callback_load[] = {0x48, 0x8b, 0x7e, 0x38};
    static const uint8_t callback_clear[] = {
        0x48, 0xc7, 0x46, 0x38, 0x00, 0x00, 0x00, 0x00,
    };
    static const uint8_t fence_id_store[] = {0x48, 0x89, 0x43, 0x48};
    static const uint8_t fence_id_load[] = {0x48, 0x8b, 0x53, 0x48};
    static const uint8_t command_fence_id_store[] = {
        0x49, 0x89, 0x54, 0x24, 0x08,
    };
    return
        has_bytes(alloc_vbufs, 0x17, alloc_slot, sizeof(alloc_slot)) &&
        has_bytes(alloc_vbufs, 0x2c, alloc_store, sizeof(alloc_store)) &&
        has_bytes(unref_resource, 0x0e, cache_load, sizeof(cache_load)) &&
        has_bytes(unref_resource, 0x25, command_base, sizeof(command_base)) &&
        has_bytes(unref_resource, 0x2c, response_base, sizeof(response_base)) &&
        has_bytes(unref_resource, 0x33, command_size, sizeof(command_size)) &&
        has_bytes(unref_resource, 0x42, response_size, sizeof(response_size)) &&
        has_bytes(unref_resource, 0x76, resource_load, sizeof(resource_load)) &&
        has_bytes(unref_resource, 0x7c, command_type, sizeof(command_type)) &&
        has_bytes(unref_resource, 0x86, callback_data, sizeof(callback_data)) &&
        has_bytes(unref_callback, 0x09, callback_load, sizeof(callback_load)) &&
        has_bytes(unref_callback, 0x0d, callback_clear, sizeof(callback_clear)) &&
        has_bytes(fence_emit, 0x42, fence_id_store, sizeof(fence_id_store)) &&
        has_bytes(fence_emit, 0xc7, fence_id_load, sizeof(fence_id_load)) &&
        has_bytes(fence_emit, 0xcb,
            command_fence_id_store, sizeof(command_fence_id_store));
}

static void drmd_virtio_gpu_unref_response_callback(void *vgdev, void *raw_vbuffer)
{
    drmd_virtio_gpu_vbuffer_t *vbuffer = raw_vbuffer;
    if (vbuffer == NULL) return;
    drmd_virtio_gpu_unref_request_t *request = vbuffer->response_callback_data;
    if (request == NULL) return;

    drmd_virtio_gpu_control_header_t response;
    memset(&response, 0, sizeof(response));
    /*
     * response_size is the descriptor capacity configured at submission.
     * Linux 6.8 discards virtqueue_get_buf()'s used length before invoking
     * resp_cb, so it cannot be treated as the returned response length.
     */
    if (vbuffer->response_buffer != NULL) {
        memcpy(&response, vbuffer->response_buffer, sizeof(response));
    }
    request->response_type = response.type;
    request->response_flags = response.flags;
    request->response_fence_id = response.fence_id;

    const uint64_t expected_fence_id = atomic_load_explicit(
        &request->expected_fence_id, memory_order_acquire);
    const int valid_response =
        response.type == DRMD_VIRTIO_GPU_RESP_OK_NODATA &&
        (response.flags & DRMD_VIRTIO_GPU_FLAG_FENCE) != 0 &&
        expected_fence_id != 0 &&
        response.fence_id == expected_fence_id;
    if (valid_response) {
        vbuffer->response_callback_data = request->object;
        request->upstream_unref_callback(vgdev, vbuffer);
    } else {
        vbuffer->response_callback_data = NULL;
    }
    atomic_store_explicit(&request->callback_done, 1u, memory_order_release);
}

int drmd_virtio_gpu_unref_bridge_init(
    drmd_virtio_gpu_unref_bridge_t *bridge,
    kb_module_t *module,
    void *vgdev)
{
    if (bridge == NULL || module == NULL || vgdev == NULL) return -22;
    memset(bridge, 0, sizeof(*bridge));
    void *queue_fenced = NULL;
    void *upstream_callback = NULL;
    void *upstream_unref_resource = NULL;
    void *upstream_alloc_vbufs = NULL;
    void *upstream_fence_emit = NULL;
    if (kb_module_find_symbol(
            module, "virtio_gpu_queue_fenced_ctrl_buffer", &queue_fenced) != KB_OK ||
        queue_fenced == NULL ||
        kb_module_find_symbol(
            module, "virtio_gpu_cmd_unref_cb", &upstream_callback) != KB_OK ||
        upstream_callback == NULL ||
        kb_module_find_symbol(
            module, "virtio_gpu_cmd_unref_resource", &upstream_unref_resource) != KB_OK ||
        upstream_unref_resource == NULL ||
        kb_module_find_symbol(
            module, "virtio_gpu_alloc_vbufs", &upstream_alloc_vbufs) != KB_OK ||
        upstream_alloc_vbufs == NULL ||
        kb_module_find_symbol(
            module, "virtio_gpu_fence_emit", &upstream_fence_emit) != KB_OK ||
        upstream_fence_emit == NULL ||
        !validate_linux_6_8_layout(
            upstream_alloc_vbufs,
            upstream_unref_resource,
            upstream_callback,
            upstream_fence_emit)) {
        return -2;
    }
    bridge->vgdev = vgdev;
    bridge->queue_fenced = (drmd_virtio_gpu_queue_fenced_fn)queue_fenced;
    bridge->upstream_unref_callback =
        (drmd_virtio_gpu_unref_callback_fn)upstream_callback;
    return 0;
}

int drmd_virtio_gpu_unref_request_submit(
    const drmd_virtio_gpu_unref_bridge_t *bridge,
    drmd_virtio_gpu_unref_request_t *request,
    void *object,
    void *fence)
{
    if (bridge == NULL || bridge->vgdev == NULL || bridge->queue_fenced == NULL ||
        bridge->upstream_unref_callback == NULL || request == NULL ||
        object == NULL || fence == NULL || request->submitted != 0 ||
        atomic_load_explicit(&request->callback_done, memory_order_acquire) != 0) {
        return -22;
    }

    void *cache = NULL;
    memcpy(&cache,
        (const uint8_t *)bridge->vgdev + DRMD_VIRTIO_GPU_VBUFS_OFFSET,
        sizeof(cache));
    if (cache == NULL) return -19;
    drmd_virtio_gpu_vbuffer_t *vbuffer =
        kb_kmem_cache_alloc(cache, DRMD_LINUX_GFP_ZERO);
    if (vbuffer == NULL) return -12;
    memset(vbuffer, 0, DRMD_VIRTIO_GPU_VBUFFER_CACHE_BYTES);

    drmd_virtio_gpu_resource_unref_t *command =
        (void *)((uint8_t *)vbuffer + DRMD_VIRTIO_GPU_VBUFFER_BYTES);
    drmd_virtio_gpu_control_header_t *response =
        (void *)((uint8_t *)command + DRMD_VIRTIO_GPU_UNREF_COMMAND_BYTES);
    vbuffer->buffer = (char *)command;
    vbuffer->size = sizeof(*command);
    vbuffer->response_buffer = (char *)response;
    vbuffer->response_size = sizeof(*response);
    vbuffer->response_callback = drmd_virtio_gpu_unref_response_callback;
    vbuffer->response_callback_data = request;
    command->header.type = DRMD_VIRTIO_GPU_CMD_RESOURCE_UNREF;
    memcpy(&command->resource_id,
        (const uint8_t *)object + DRMD_VIRTIO_GPU_OBJECT_RESOURCE_ID_OFFSET,
        sizeof(command->resource_id));

    request->object = object;
    request->fence = fence;
    request->upstream_unref_callback = bridge->upstream_unref_callback;
    request->submitted = 1;
    const int status = bridge->queue_fenced(
        bridge->vgdev, vbuffer, fence);
    if (status < 0) {
        memset(request, 0, sizeof(*request));
        return status;
    }
    uint64_t expected_fence_id = 0;
    memcpy(&expected_fence_id,
        (const uint8_t *)fence + DRMD_VIRTIO_GPU_FENCE_ID_OFFSET,
        sizeof(expected_fence_id));
    atomic_store_explicit(
        &request->expected_fence_id, expected_fence_id, memory_order_release);
    return 0;
}

int drmd_virtio_gpu_unref_request_poll(
    const drmd_virtio_gpu_unref_request_t *request,
    drmd_virtio_gpu_unref_result_t *out_result)
{
    if (request == NULL || out_result == NULL || request->submitted == 0) return -22;
    if (atomic_load_explicit(
            &request->callback_done, memory_order_acquire) == 0) return 0;
    out_result->response_type = request->response_type;
    out_result->response_flags = request->response_flags;
    out_result->response_fence_id = request->response_fence_id;
    if (request->response_type != DRMD_VIRTIO_GPU_RESP_OK_NODATA ||
        (request->response_flags & DRMD_VIRTIO_GPU_FLAG_FENCE) == 0 ||
        request->response_fence_id == 0 ||
        request->response_fence_id != atomic_load_explicit(
            &request->expected_fence_id, memory_order_acquire)) {
        return -5;
    }
    return 1;
}
