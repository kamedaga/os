#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../userland/drmd/src/virtio_gpu_unref_bridge.h"

enum {
    TEST_VGDEV_BYTES = 0xf240,
    TEST_OBJECT_BYTES = 0x2c0,
    TEST_FENCE_BYTES = 0x78,
    TEST_VBUFFER_SLOT_BYTES = 0xd8,
    TEST_VGDEV_VBUFS_OFFSET = 0xf238,
    TEST_OBJECT_RESOURCE_ID_OFFSET = 0x198,
    TEST_FENCE_ID_OFFSET = 0x48,
    TEST_COMMAND_OFFSET = 0x60,
    TEST_RESPONSE_OFFSET = 0x80,
    TEST_RESPONSE_CALLBACK_OFFSET = 0x30,
    TEST_RESPONSE_CALLBACK_DATA_OFFSET = 0x38,
};

static unsigned char test_vbuffer[TEST_VBUFFER_SLOT_BYTES];
static void *queued_vbuffer;
static int queue_status;
static uint64_t issued_fence_id;
static int cleanup_calls;
static int failures;

static int expect(int condition, const char *message)
{
    if (condition) return 0;
    fprintf(stderr, "DRMD_UNREF_BRIDGE_FAIL %s\n", message);
    failures++;
    return -1;
}

static void fake_upstream_unref_callback(void *vgdev, void *vbuffer)
{
    (void)vgdev;
    void *object = NULL;
    memcpy(&object,
        (unsigned char *)vbuffer + TEST_RESPONSE_CALLBACK_DATA_OFFSET,
        sizeof(object));
    expect(object != NULL, "upstream callback did not receive object");
    object = NULL;
    memcpy((unsigned char *)vbuffer + TEST_RESPONSE_CALLBACK_DATA_OFFSET,
        &object, sizeof(object));
    cleanup_calls++;
}

static int fake_queue_fenced(void *vgdev, void *vbuffer, void *fence)
{
    (void)vgdev;
    queued_vbuffer = vbuffer;
    uint32_t command_type = 0;
    uint32_t resource_id = 0;
    memcpy(&command_type,
        (unsigned char *)vbuffer + TEST_COMMAND_OFFSET,
        sizeof(command_type));
    memcpy(&resource_id,
        (unsigned char *)vbuffer + TEST_COMMAND_OFFSET + 0x18,
        sizeof(resource_id));
    expect(command_type == 0x0102, "wrong RESOURCE_UNREF command type");
    expect(resource_id == 73, "wrong RESOURCE_UNREF resource id");
    if (queue_status == 0) {
        memcpy((unsigned char *)fence + TEST_FENCE_ID_OFFSET,
            &issued_fence_id, sizeof(issued_fence_id));
    }
    return queue_status;
}

kb_status_t kb_module_find_symbol(
    kb_module_t *module,
    const char *name,
    void **out_address)
{
    (void)module;
    (void)name;
    if (out_address == NULL) return KB_ERR_INVALID;
    *out_address = NULL;
    return KB_ERR_NOT_FOUND;
}

void *kb_kmem_cache_alloc(void *cache, unsigned int flags)
{
    expect(cache == (void *)(uintptr_t)0x1234, "wrong vbuffer cache pointer");
    expect((flags & 0x100u) != 0, "vbuffer allocation omitted zero flag");
    memset(test_vbuffer, 0, sizeof(test_vbuffer));
    return test_vbuffer;
}

static void complete_response(uint32_t type, uint32_t flags, uint64_t fence_id)
{
    unsigned char *response = (unsigned char *)queued_vbuffer + TEST_RESPONSE_OFFSET;
    memcpy(response, &type, sizeof(type));
    memcpy(response + 4, &flags, sizeof(flags));
    memcpy(response + 8, &fence_id, sizeof(fence_id));
    void (*callback)(void *, void *) = NULL;
    memcpy(&callback,
        (unsigned char *)queued_vbuffer + TEST_RESPONSE_CALLBACK_OFFSET,
        sizeof(callback));
    expect(callback != NULL, "response callback missing");
    callback(NULL, queued_vbuffer);
}

int main(void)
{
    unsigned char vgdev[TEST_VGDEV_BYTES];
    unsigned char object[TEST_OBJECT_BYTES];
    unsigned char fence[TEST_FENCE_BYTES];
    memset(vgdev, 0, sizeof(vgdev));
    memset(object, 0, sizeof(object));
    memset(fence, 0, sizeof(fence));
    void *cache = (void *)(uintptr_t)0x1234;
    memcpy(vgdev + TEST_VGDEV_VBUFS_OFFSET, &cache, sizeof(cache));
    const uint32_t resource_id = 73;
    memcpy(object + TEST_OBJECT_RESOURCE_ID_OFFSET,
        &resource_id, sizeof(resource_id));

    drmd_virtio_gpu_unref_bridge_t bridge = {
        .vgdev = vgdev,
        .queue_fenced = fake_queue_fenced,
        .upstream_unref_callback = fake_upstream_unref_callback,
    };

    drmd_virtio_gpu_unref_request_t request;
    memset(&request, 0, sizeof(request));
    drmd_virtio_gpu_unref_result_t result;
    memset(&result, 0, sizeof(result));
    queue_status = 0;
    issued_fence_id = 19;
    expect(drmd_virtio_gpu_unref_request_submit(
        &bridge, &request, object, fence) == 0,
        "valid request submission failed");
    expect(drmd_virtio_gpu_unref_request_poll(&request, &result) == 0,
        "request completed before callback");
    expect(atomic_load_explicit(
        &request.expected_fence_id, memory_order_acquire) == issued_fence_id,
        "issued fence id was not captured");
    complete_response(0x1100, 1, 19);
    expect(cleanup_calls == 1, "upstream cleanup callback count mismatch");
    expect(drmd_virtio_gpu_unref_request_poll(&request, &result) == 1,
        "exact fenced response rejected");
    expect(result.response_fence_id == 19,
        "response fence id changed");

    memset(&request, 0, sizeof(request));
    queued_vbuffer = NULL;
    issued_fence_id = 20;
    expect(drmd_virtio_gpu_unref_request_submit(
        &bridge, &request, object, fence) == 0,
        "mismatched response request submission failed");
    complete_response(0x1100, 1, 21);
    expect(cleanup_calls == 1,
        "mismatched response fence released resource id");
    expect(drmd_virtio_gpu_unref_request_poll(&request, &result) == -5,
        "mismatched response fence accepted");

    memset(&request, 0, sizeof(request));
    queued_vbuffer = NULL;
    issued_fence_id = 22;
    expect(drmd_virtio_gpu_unref_request_submit(
        &bridge, &request, object, fence) == 0,
        "error response request submission failed");
    complete_response(0x1200, 1, 22);
    expect(cleanup_calls == 1,
        "error response released resource id");
    expect(drmd_virtio_gpu_unref_request_poll(&request, &result) == -5,
        "error response accepted");

    memset(&request, 0, sizeof(request));
    queued_vbuffer = NULL;
    queue_status = -19;
    issued_fence_id = 23;
    expect(drmd_virtio_gpu_unref_request_submit(
        &bridge, &request, object, fence) == -19,
        "queue failure hidden");
    expect(drmd_virtio_gpu_unref_request_poll(&request, &result) == -22,
        "unqueued request became pending");

    if (failures != 0) return 1;
    puts("DRMD_UNREF_BRIDGE_PASS");
    return 0;
}
