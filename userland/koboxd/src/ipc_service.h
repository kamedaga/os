#pragma once

#include "pacha/ipc.h"

#include <stdint.h>
#include <stdio.h>

enum {
    KOBOXD_IPC_PROTOCOL_MAGIC = 0x314950494458424bull,
    KOBOXD_IPC_PROTOCOL_VERSION = 1,
    KOBOXD_IPC_QUEUE_CAPACITY = 64,
    KOBOXD_IPC_INLINE_BYTES = 128,
    KOBOXD_IPC_INVALID_REQUEST_ID = 0,
};

typedef enum koboxd_ipc_endpoint_kind {
    KOBOXD_IPC_ENDPOINT_CONTROL = 1,
    KOBOXD_IPC_ENDPOINT_BLOCK = 2,
    KOBOXD_IPC_ENDPOINT_FS_BACKEND = 3,
    KOBOXD_IPC_ENDPOINT_EVENT = 4,
} koboxd_ipc_endpoint_kind_t;

typedef enum koboxd_ipc_transport {
    KOBOXD_IPC_TRANSPORT_NORMAL = 0,
    KOBOXD_IPC_TRANSPORT_PKEY_DATA_PLANE = 1,
} koboxd_ipc_transport_t;

typedef enum koboxd_ipc_request_flags {
    KOBOXD_IPC_REQUEST_F_EXPECT_REPLY = 1u << 0,
    KOBOXD_IPC_REQUEST_F_ALLOW_PKEY_DATA = 1u << 1,
    KOBOXD_IPC_REQUEST_F_CANCELLED = 1u << 2,
} koboxd_ipc_request_flags_t;

typedef enum koboxd_control_op {
    KOBOXD_CONTROL_HELLO = 1,
    KOBOXD_CONTROL_GET_ENDPOINT = 2,
    KOBOXD_CONTROL_SETUP_PKEY_DATA_PLANE = 3,
    KOBOXD_CONTROL_CANCEL = 4,
    KOBOXD_CONTROL_GET_METRICS = 5,
    KOBOXD_CONTROL_DEBUG_DUMP = 6,
} koboxd_control_op_t;

typedef enum koboxd_block_op {
    KOBOXD_BLOCK_IDENTIFY = 1,
    KOBOXD_BLOCK_READ = 2,
    KOBOXD_BLOCK_WRITE = 3,
    KOBOXD_BLOCK_FLUSH = 4,
} koboxd_block_op_t;

typedef enum koboxd_fs_backend_op {
    KOBOXD_FS_MOUNT_ROOT = 1,
    KOBOXD_FS_LOOKUP = 2,
    KOBOXD_FS_OPEN = 3,
    KOBOXD_FS_PREAD = 4,
    KOBOXD_FS_PWRITE = 5,
    KOBOXD_FS_STATX = 6,
    KOBOXD_FS_GETDENTS = 7,
    KOBOXD_FS_FSYNC = 8,
    KOBOXD_FS_UNLINK = 9,
    KOBOXD_FS_RENAME = 10,
} koboxd_fs_backend_op_t;

typedef enum koboxd_event_op {
    KOBOXD_EVENT_SUBSCRIBE = 1,
    KOBOXD_EVENT_UNSUBSCRIBE = 2,
    KOBOXD_EVENT_NEXT = 3,
} koboxd_event_op_t;

typedef struct koboxd_ipc_header {
    uint64_t magic;
    uint32_t version;
    uint16_t header_bytes;
    uint16_t endpoint_kind;
    uint32_t op;
    uint32_t flags;
    uint64_t request_id;
    uint64_t timeout_ns;
    uint64_t object_id;
    uint64_t offset;
    uint64_t length;
    uint64_t data_plane_slot;
    uint64_t data_plane_offset;
    uint64_t data_plane_length;
    uint64_t inline_bytes;
} koboxd_ipc_header_t;

typedef struct koboxd_ipc_reply_header {
    uint64_t magic;
    uint32_t version;
    uint16_t header_bytes;
    uint16_t endpoint_kind;
    uint32_t op;
    int32_t status;
    uint64_t request_id;
    uint64_t result0;
    uint64_t result1;
    uint64_t data_plane_slot;
    uint64_t data_plane_offset;
    uint64_t data_plane_length;
    uint64_t inline_bytes;
} koboxd_ipc_reply_header_t;

typedef struct koboxd_ipc_request {
    koboxd_ipc_header_t header;
    uint8_t inline_payload[KOBOXD_IPC_INLINE_BYTES];
} koboxd_ipc_request_t;

typedef struct koboxd_ipc_reply {
    koboxd_ipc_reply_header_t header;
    uint8_t inline_payload[KOBOXD_IPC_INLINE_BYTES];
} koboxd_ipc_reply_t;

typedef struct koboxd_ipc_queue_item {
    koboxd_ipc_request_t request;
    uint64_t enqueued_tick;
    uint8_t active;
    uint8_t cancelled;
} koboxd_ipc_queue_item_t;

typedef struct koboxd_ipc_worker_queue {
    koboxd_ipc_queue_item_t items[KOBOXD_IPC_QUEUE_CAPACITY];
    uint64_t head;
    uint64_t tail;
    uint64_t depth;
} koboxd_ipc_worker_queue_t;

typedef struct koboxd_ipc_metrics {
    uint64_t enqueued;
    uint64_t dequeued;
    uint64_t completed;
    uint64_t cancelled;
    uint64_t timed_out;
    uint64_t queue_full;
    uint64_t invalid;
    uint64_t normal_fallback;
    uint64_t pkey_data_plane;
    uint64_t bytes_in;
    uint64_t bytes_out;
} koboxd_ipc_metrics_t;

typedef struct koboxd_ipc_endpoint {
    koboxd_ipc_endpoint_kind_t kind;
    const char *name;
    int endpoint_fd;
    int event_fd;
    uint8_t ready;
    uint8_t pkey_allowed;
    uint32_t pkey;
    uint64_t next_request_id;
    struct pacha_ipc_fast_channel fast;
    koboxd_ipc_worker_queue_t queue;
    koboxd_ipc_metrics_t metrics;
} koboxd_ipc_endpoint_t;

typedef struct koboxd_ipc_service {
    koboxd_ipc_endpoint_t control;
    koboxd_ipc_endpoint_t block;
    koboxd_ipc_endpoint_t fs_backend;
    koboxd_ipc_endpoint_t event;
} koboxd_ipc_service_t;

typedef int (*koboxd_ipc_endpoint_handler_fn)(
    void *ctx,
    const koboxd_ipc_request_t *request,
    koboxd_ipc_reply_t *reply);

void koboxd_ipc_service_init(koboxd_ipc_service_t *service);
void koboxd_ipc_service_mark_storage_ready(koboxd_ipc_service_t *service);
void koboxd_ipc_service_debug_dump(const koboxd_ipc_service_t *service, FILE *out);

koboxd_ipc_endpoint_t *koboxd_ipc_service_endpoint(koboxd_ipc_service_t *service, koboxd_ipc_endpoint_kind_t kind);
const koboxd_ipc_endpoint_t *koboxd_ipc_service_endpoint_const(const koboxd_ipc_service_t *service, koboxd_ipc_endpoint_kind_t kind);
const char *koboxd_ipc_endpoint_kind_name(koboxd_ipc_endpoint_kind_t kind);
const char *koboxd_ipc_transport_name(koboxd_ipc_transport_t transport);

int koboxd_ipc_make_request(
    koboxd_ipc_endpoint_t *endpoint,
    uint32_t op,
    uint32_t flags,
    uint64_t timeout_ns,
    const void *inline_payload,
    uint64_t inline_bytes,
    koboxd_ipc_request_t *out_request);
int koboxd_ipc_make_reply(
    const koboxd_ipc_request_t *request,
    int32_t status,
    uint64_t result0,
    uint64_t result1,
    const void *inline_payload,
    uint64_t inline_bytes,
    koboxd_ipc_reply_t *out_reply);
int koboxd_ipc_validate_request(const koboxd_ipc_request_t *request, koboxd_ipc_endpoint_kind_t expected_kind);
int koboxd_ipc_validate_reply(const koboxd_ipc_reply_t *reply, const koboxd_ipc_request_t *request);

koboxd_ipc_transport_t koboxd_ipc_endpoint_transport(koboxd_ipc_endpoint_t *endpoint, uint32_t request_flags);
int koboxd_ipc_endpoint_enqueue(koboxd_ipc_endpoint_t *endpoint, const koboxd_ipc_request_t *request, uint64_t now_tick);
int koboxd_ipc_endpoint_dequeue(koboxd_ipc_endpoint_t *endpoint, koboxd_ipc_queue_item_t *out_item);
int koboxd_ipc_endpoint_cancel(koboxd_ipc_endpoint_t *endpoint, uint64_t request_id);
void koboxd_ipc_endpoint_complete(koboxd_ipc_endpoint_t *endpoint, const koboxd_ipc_request_t *request, const koboxd_ipc_reply_t *reply);
void koboxd_ipc_endpoint_record_timeout(koboxd_ipc_endpoint_t *endpoint, uint64_t request_id);
int koboxd_ipc_endpoint_call_local(
    koboxd_ipc_endpoint_t *endpoint,
    const koboxd_ipc_request_t *request,
    uint64_t now_tick,
    koboxd_ipc_endpoint_handler_fn handler,
    void *ctx,
    koboxd_ipc_reply_t *out_reply);
