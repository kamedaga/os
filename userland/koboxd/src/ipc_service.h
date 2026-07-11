#pragma once

#include "pacha/service_abi.h"
#include "pacha/ipc.h"

#include <stdint.h>
#include <stdio.h>

enum {
    KOBOXD_IPC_QUEUE_CAPACITY = 64,
    KOBOXD_IPC_INLINE_BYTES = 128,
    KOBOXD_IPC_INVALID_REQUEST_ID = 0,
};

typedef enum koboxd_ipc_endpoint_kind {
    KOBOXD_IPC_ENDPOINT_CONTROL = 1,
    KOBOXD_IPC_ENDPOINT_BLOCK = 2,
    KOBOXD_IPC_ENDPOINT_FS_BACKEND = 3,
    KOBOXD_IPC_ENDPOINT_EVENT = 4,
    KOBOXD_IPC_ENDPOINT_FILED = 5,
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
    KOBOXD_IPC_CONTROL_OP_HELLO = 0u,
    KOBOXD_IPC_CONTROL_OP_GET_ENDPOINT = 1u,
    KOBOXD_IPC_CONTROL_OP_SETUP_PKEY_DATA_PLANE = 2u,
    KOBOXD_IPC_CONTROL_OP_CANCEL = 3u,
    KOBOXD_IPC_CONTROL_OP_GET_METRICS = 4u,
    KOBOXD_IPC_CONTROL_OP_DEBUG_DUMP = 5u,
} koboxd_control_op_t;

typedef enum koboxd_block_op {
    KOBOXD_IPC_BLOCK_OP_IDENTIFY = 0u,
    KOBOXD_IPC_BLOCK_OP_READ = 1u,
    KOBOXD_IPC_BLOCK_OP_WRITE = 2u,
    KOBOXD_IPC_BLOCK_OP_FLUSH = 3u,
} koboxd_block_op_t;

typedef enum koboxd_fs_backend_op {
    KOBOXD_IPC_FS_OP_MOUNT_ROOT = 0u,
    KOBOXD_IPC_FS_OP_LOOKUP = 1u,
    KOBOXD_IPC_FS_OP_STATX = 2u,
    KOBOXD_IPC_FS_OP_GETDENTS = 3u,
    KOBOXD_IPC_FS_OP_PREAD = 4u,
    KOBOXD_IPC_FS_OP_PWRITE = 5u,
    KOBOXD_IPC_FS_OP_FSYNC = 6u,
    KOBOXD_IPC_FS_OP_CREATE = 7u,
    KOBOXD_IPC_FS_OP_TRUNCATE = 8u,
    KOBOXD_IPC_FS_OP_UNLINK = 9u,
    KOBOXD_IPC_FS_OP_RENAME = 10u,
} koboxd_fs_backend_op_t;

typedef enum koboxd_event_op {
    KOBOXD_IPC_EVENT_OP_SUBSCRIBE = 0u,
    KOBOXD_IPC_EVENT_OP_UNSUBSCRIBE = 1u,
    KOBOXD_IPC_EVENT_OP_NEXT = 2u,
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
    koboxd_ipc_endpoint_t filed;
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
