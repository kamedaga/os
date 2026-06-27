#include "ipc_service.h"

#include <stdio.h>
#include <string.h>

static void endpoint_init(
    koboxd_ipc_endpoint_t *endpoint,
    koboxd_ipc_endpoint_kind_t kind,
    const char *name,
    uint8_t pkey_allowed)
{
    memset(endpoint, 0, sizeof(*endpoint));
    endpoint->kind = kind;
    endpoint->name = name;
    endpoint->endpoint_fd = -1;
    endpoint->event_fd = -1;
    endpoint->ready = 0;
    endpoint->pkey_allowed = pkey_allowed;
    endpoint->pkey = 0;
    endpoint->next_request_id = 1;
    (void)pacha_ipc_fast_channel_init_normal(&endpoint->fast, -1);
}

void koboxd_ipc_service_init(koboxd_ipc_service_t *service)
{
    if (service == NULL) {
        return;
    }
    endpoint_init(&service->control, KOBOXD_IPC_ENDPOINT_CONTROL, "control", 0);
    endpoint_init(&service->block, KOBOXD_IPC_ENDPOINT_BLOCK, "block", 1);
    endpoint_init(&service->fs_backend, KOBOXD_IPC_ENDPOINT_FS_BACKEND, "fs-backend", 1);
    endpoint_init(&service->event, KOBOXD_IPC_ENDPOINT_EVENT, "event", 0);
    endpoint_init(&service->filed, KOBOXD_IPC_ENDPOINT_FILED, "filed", 0);
    service->control.ready = 1;
}

void koboxd_ipc_service_mark_storage_ready(koboxd_ipc_service_t *service)
{
    if (service == NULL) {
        return;
    }
    service->block.ready = 1;
    service->fs_backend.ready = 1;
    service->event.ready = 1;
    service->filed.ready = 1;
}

koboxd_ipc_endpoint_t *koboxd_ipc_service_endpoint(koboxd_ipc_service_t *service, koboxd_ipc_endpoint_kind_t kind)
{
    if (service == NULL) {
        return NULL;
    }
    switch (kind) {
    case KOBOXD_IPC_ENDPOINT_CONTROL: return &service->control;
    case KOBOXD_IPC_ENDPOINT_BLOCK: return &service->block;
    case KOBOXD_IPC_ENDPOINT_FS_BACKEND: return &service->fs_backend;
    case KOBOXD_IPC_ENDPOINT_EVENT: return &service->event;
    case KOBOXD_IPC_ENDPOINT_FILED: return &service->filed;
    default: return NULL;
    }
}

const koboxd_ipc_endpoint_t *koboxd_ipc_service_endpoint_const(
    const koboxd_ipc_service_t *service,
    koboxd_ipc_endpoint_kind_t kind)
{
    return koboxd_ipc_service_endpoint((koboxd_ipc_service_t *)(uintptr_t)service, kind);
}

const char *koboxd_ipc_endpoint_kind_name(koboxd_ipc_endpoint_kind_t kind)
{
    switch (kind) {
    case KOBOXD_IPC_ENDPOINT_CONTROL: return "control";
    case KOBOXD_IPC_ENDPOINT_BLOCK: return "block";
    case KOBOXD_IPC_ENDPOINT_FS_BACKEND: return "fs-backend";
    case KOBOXD_IPC_ENDPOINT_EVENT: return "event";
    case KOBOXD_IPC_ENDPOINT_FILED: return "filed";
    default: return "unknown";
    }
}

const char *koboxd_ipc_transport_name(koboxd_ipc_transport_t transport)
{
    switch (transport) {
    case KOBOXD_IPC_TRANSPORT_NORMAL: return "normal";
    case KOBOXD_IPC_TRANSPORT_PKEY_DATA_PLANE: return "pkey-data-plane";
    default: return "unknown";
    }
}

static void dump_endpoint(const koboxd_ipc_endpoint_t *endpoint, FILE *out)
{
    if (endpoint == NULL || out == NULL) {
        return;
    }
    fprintf(out,
        "[koboxd] ipc endpoint=%s ready=%u fd=%d event_fd=%d fast=%u queue_depth=%llu enq=%llu deq=%llu done=%llu cancel=%llu timeout=%llu full=%llu invalid=%llu pkey=%llu normal=%llu bytes_in=%llu bytes_out=%llu\n",
        endpoint->name,
        (unsigned)endpoint->ready,
        endpoint->endpoint_fd,
        endpoint->event_fd,
        (unsigned)pacha_ipc_fast_channel_uses_ring(&endpoint->fast),
        (unsigned long long)endpoint->queue.depth,
        (unsigned long long)endpoint->metrics.enqueued,
        (unsigned long long)endpoint->metrics.dequeued,
        (unsigned long long)endpoint->metrics.completed,
        (unsigned long long)endpoint->metrics.cancelled,
        (unsigned long long)endpoint->metrics.timed_out,
        (unsigned long long)endpoint->metrics.queue_full,
        (unsigned long long)endpoint->metrics.invalid,
        (unsigned long long)endpoint->metrics.pkey_data_plane,
        (unsigned long long)endpoint->metrics.normal_fallback,
        (unsigned long long)endpoint->metrics.bytes_in,
        (unsigned long long)endpoint->metrics.bytes_out);
}

void koboxd_ipc_service_debug_dump(const koboxd_ipc_service_t *service, FILE *out)
{
    if (service == NULL || out == NULL) {
        return;
    }
    dump_endpoint(&service->control, out);
    dump_endpoint(&service->block, out);
    dump_endpoint(&service->fs_backend, out);
    dump_endpoint(&service->event, out);
    dump_endpoint(&service->filed, out);
}

int koboxd_ipc_make_request(
    koboxd_ipc_endpoint_t *endpoint,
    uint32_t op,
    uint32_t flags,
    uint64_t timeout_ns,
    const void *inline_payload,
    uint64_t inline_bytes,
    koboxd_ipc_request_t *out_request)
{
    if (endpoint == NULL || out_request == NULL || inline_bytes > KOBOXD_IPC_INLINE_BYTES) {
        return -1;
    }
    if (inline_bytes != 0 && inline_payload == NULL) {
        return -2;
    }
    const uint64_t request_id = endpoint->next_request_id++;
    if (endpoint->next_request_id == KOBOXD_IPC_INVALID_REQUEST_ID) {
        endpoint->next_request_id = 1;
    }
    memset(out_request, 0, sizeof(*out_request));
    out_request->header.magic = KOBOXD_IPC_PROTOCOL_MAGIC;
    out_request->header.version = KOBOXD_IPC_PROTOCOL_VERSION;
    out_request->header.header_bytes = sizeof(out_request->header);
    out_request->header.endpoint_kind = (uint16_t)endpoint->kind;
    out_request->header.op = op;
    out_request->header.flags = flags;
    out_request->header.request_id = request_id;
    out_request->header.timeout_ns = timeout_ns;
    out_request->header.inline_bytes = inline_bytes;
    if (inline_bytes != 0) {
        memcpy(out_request->inline_payload, inline_payload, inline_bytes);
    }
    return 0;
}

int koboxd_ipc_make_reply(
    const koboxd_ipc_request_t *request,
    int32_t status,
    uint64_t result0,
    uint64_t result1,
    const void *inline_payload,
    uint64_t inline_bytes,
    koboxd_ipc_reply_t *out_reply)
{
    if (request == NULL || out_reply == NULL || inline_bytes > KOBOXD_IPC_INLINE_BYTES) {
        return -1;
    }
    if (inline_bytes != 0 && inline_payload == NULL) {
        return -2;
    }
    memset(out_reply, 0, sizeof(*out_reply));
    out_reply->header.magic = KOBOXD_IPC_PROTOCOL_MAGIC;
    out_reply->header.version = KOBOXD_IPC_PROTOCOL_VERSION;
    out_reply->header.header_bytes = sizeof(out_reply->header);
    out_reply->header.endpoint_kind = request->header.endpoint_kind;
    out_reply->header.op = request->header.op;
    out_reply->header.status = status;
    out_reply->header.request_id = request->header.request_id;
    out_reply->header.result0 = result0;
    out_reply->header.result1 = result1;
    out_reply->header.data_plane_slot = request->header.data_plane_slot;
    out_reply->header.data_plane_offset = request->header.data_plane_offset;
    out_reply->header.data_plane_length = request->header.data_plane_length;
    out_reply->header.inline_bytes = inline_bytes;
    if (inline_bytes != 0) {
        memcpy(out_reply->inline_payload, inline_payload, inline_bytes);
    }
    return 0;
}

int koboxd_ipc_validate_request(const koboxd_ipc_request_t *request, koboxd_ipc_endpoint_kind_t expected_kind)
{
    if (request == NULL) {
        return -1;
    }
    if (request->header.magic != KOBOXD_IPC_PROTOCOL_MAGIC ||
        request->header.version != KOBOXD_IPC_PROTOCOL_VERSION ||
        request->header.header_bytes != sizeof(request->header))
    {
        return -2;
    }
    if (request->header.endpoint_kind != (uint16_t)expected_kind ||
        request->header.request_id == KOBOXD_IPC_INVALID_REQUEST_ID ||
        request->header.inline_bytes > KOBOXD_IPC_INLINE_BYTES)
    {
        return -3;
    }
    return 0;
}

int koboxd_ipc_validate_reply(const koboxd_ipc_reply_t *reply, const koboxd_ipc_request_t *request)
{
    if (reply == NULL || request == NULL) {
        return -1;
    }
    if (reply->header.magic != KOBOXD_IPC_PROTOCOL_MAGIC ||
        reply->header.version != KOBOXD_IPC_PROTOCOL_VERSION ||
        reply->header.header_bytes != sizeof(reply->header))
    {
        return -2;
    }
    if (reply->header.endpoint_kind != request->header.endpoint_kind ||
        reply->header.op != request->header.op ||
        reply->header.request_id != request->header.request_id ||
        reply->header.inline_bytes > KOBOXD_IPC_INLINE_BYTES)
    {
        return -3;
    }
    return 0;
}

koboxd_ipc_transport_t koboxd_ipc_endpoint_transport(koboxd_ipc_endpoint_t *endpoint, uint32_t request_flags)
{
    if (endpoint == NULL ||
        !endpoint->ready ||
        !endpoint->pkey_allowed ||
        (request_flags & KOBOXD_IPC_REQUEST_F_ALLOW_PKEY_DATA) == 0 ||
        !pacha_ipc_fast_channel_uses_ring(&endpoint->fast))
    {
        if (endpoint != NULL) {
            endpoint->metrics.normal_fallback++;
        }
        return KOBOXD_IPC_TRANSPORT_NORMAL;
    }
    endpoint->metrics.pkey_data_plane++;
    return KOBOXD_IPC_TRANSPORT_PKEY_DATA_PLANE;
}

int koboxd_ipc_endpoint_enqueue(koboxd_ipc_endpoint_t *endpoint, const koboxd_ipc_request_t *request, uint64_t now_tick)
{
    if (endpoint == NULL || request == NULL) {
        return -1;
    }
    const int valid = koboxd_ipc_validate_request(request, endpoint->kind);
    if (valid != 0) {
        endpoint->metrics.invalid++;
        return valid;
    }
    if (endpoint->queue.depth >= KOBOXD_IPC_QUEUE_CAPACITY) {
        endpoint->metrics.queue_full++;
        return -4;
    }
    const uint64_t slot = endpoint->queue.tail % KOBOXD_IPC_QUEUE_CAPACITY;
    koboxd_ipc_queue_item_t *item = &endpoint->queue.items[slot];
    memset(item, 0, sizeof(*item));
    item->request = *request;
    item->enqueued_tick = now_tick;
    item->active = 1;
    endpoint->queue.tail++;
    endpoint->queue.depth++;
    endpoint->metrics.enqueued++;
    endpoint->metrics.bytes_in += request->header.inline_bytes + request->header.data_plane_length;
    return 0;
}

int koboxd_ipc_endpoint_dequeue(koboxd_ipc_endpoint_t *endpoint, koboxd_ipc_queue_item_t *out_item)
{
    if (endpoint == NULL || out_item == NULL) {
        return -1;
    }
    if (endpoint->queue.depth == 0) {
        return -2;
    }
    const uint64_t slot = endpoint->queue.head % KOBOXD_IPC_QUEUE_CAPACITY;
    koboxd_ipc_queue_item_t *item = &endpoint->queue.items[slot];
    *out_item = *item;
    memset(item, 0, sizeof(*item));
    endpoint->queue.head++;
    endpoint->queue.depth--;
    endpoint->metrics.dequeued++;
    return 0;
}

int koboxd_ipc_endpoint_cancel(koboxd_ipc_endpoint_t *endpoint, uint64_t request_id)
{
    if (endpoint == NULL || request_id == KOBOXD_IPC_INVALID_REQUEST_ID) {
        return -1;
    }
    for (uint64_t i = 0; i < endpoint->queue.depth; i++) {
        const uint64_t slot = (endpoint->queue.head + i) % KOBOXD_IPC_QUEUE_CAPACITY;
        koboxd_ipc_queue_item_t *item = &endpoint->queue.items[slot];
        if (item->active && item->request.header.request_id == request_id) {
            item->cancelled = 1;
            item->request.header.flags |= KOBOXD_IPC_REQUEST_F_CANCELLED;
            endpoint->metrics.cancelled++;
            return 0;
        }
    }
    return -2;
}

void koboxd_ipc_endpoint_complete(
    koboxd_ipc_endpoint_t *endpoint,
    const koboxd_ipc_request_t *request,
    const koboxd_ipc_reply_t *reply)
{
    if (endpoint == NULL || request == NULL || reply == NULL) {
        return;
    }
    endpoint->metrics.completed++;
    endpoint->metrics.bytes_out += reply->header.inline_bytes + reply->header.data_plane_length;
}

void koboxd_ipc_endpoint_record_timeout(koboxd_ipc_endpoint_t *endpoint, uint64_t request_id)
{
    if (endpoint == NULL || request_id == KOBOXD_IPC_INVALID_REQUEST_ID) {
        return;
    }
    endpoint->metrics.timed_out++;
}

int koboxd_ipc_endpoint_call_local(
    koboxd_ipc_endpoint_t *endpoint,
    const koboxd_ipc_request_t *request,
    uint64_t now_tick,
    koboxd_ipc_endpoint_handler_fn handler,
    void *ctx,
    koboxd_ipc_reply_t *out_reply)
{
    if (endpoint == NULL || request == NULL || handler == NULL || out_reply == NULL) {
        return -1;
    }
    (void)koboxd_ipc_endpoint_transport(endpoint, request->header.flags);
    int status = koboxd_ipc_endpoint_enqueue(endpoint, request, now_tick);
    if (status != 0) {
        return status;
    }

    koboxd_ipc_queue_item_t item;
    status = koboxd_ipc_endpoint_dequeue(endpoint, &item);
    if (status != 0) {
        return status;
    }
    if (item.cancelled || (item.request.header.flags & KOBOXD_IPC_REQUEST_F_CANCELLED) != 0) {
        status = koboxd_ipc_make_reply(&item.request, -125, 0, 0, NULL, 0, out_reply);
        if (status == 0) {
            koboxd_ipc_endpoint_complete(endpoint, &item.request, out_reply);
        }
        return -125;
    }

    status = handler(ctx, &item.request, out_reply);
    if (status == 0) {
        status = koboxd_ipc_validate_reply(out_reply, &item.request);
    }
    if (status == 0) {
        koboxd_ipc_endpoint_complete(endpoint, &item.request, out_reply);
    } else {
        endpoint->metrics.invalid++;
    }
    return status;
}
