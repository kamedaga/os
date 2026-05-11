#ifndef CAPABILITYOS_ESP_SERVER_BLOCK_CLIENT_H
#define CAPABILITYOS_ESP_SERVER_BLOCK_CLIENT_H

#include "fs_server_abi.h"

enum {
    BLOCK_PAGE_BYTES = 4096,
    BLOCK_REQUEST_MAGIC = 0x514B4C42u,
    BLOCK_RESPONSE_MAGIC = 0x524B4C42u,
    BLOCK_PROTOCOL_VERSION = 1,
    BLOCK_OP_CONNECT = 1,
    BLOCK_OP_IDENTIFY = 2,
    BLOCK_OP_READ_BLOCKS = 3,
    BLOCK_STATUS_OK = 0,
    SERVICE_REGISTRY_MAGIC = 0x53525643u,
    SERVICE_REGISTRY_VERSION = 1,
    SERVICE_KIND_BLOCK = 4,
    SERVICE_FLAG_PROCESS_SLOT_COMPAT = 1,
};

struct block_request_header {
    u32 magic;
    u16 version;
    u16 op;
    u64 request_seq;
    u64 object_token;
    u64 block_index;
    u32 block_count;
    u32 flags;
    u16 inline_bytes;
    u16 reserved0;
    u32 reserved1;
    u64 arg0;
    u64 arg1;
    u64 session_nonce;
};

struct block_response_header {
    u32 magic;
    u16 version;
    u16 op;
    u64 response_seq;
    i32 status;
    u32 result_flags;
    u64 result_token;
    u16 inline_bytes;
    u8 object_kind;
    u8 reserved0;
    u32 reserved1;
    u64 arg0;
    u64 arg1;
};

struct service_registry_entry {
    u64 kind;
    u64 process_slot;
    u64 endpoint_id;
    u64 flags;
};

struct service_registry_page {
    u64 magic;
    u64 version;
    u64 entry_count;
    u64 reserved0;
    struct service_registry_entry entries[8];
};

struct block_client {
    u64 request_va;
    u64 response_va;
    u64 request_paddr;
    u64 response_paddr;
    u64 endpoint_id;
    u64 server_process_slot;
    u64 session_nonce;
    u64 root_token;
    u64 block_size;
    u64 capacity_blocks;
    u64 next_seq;
};

int block_client_connect(struct block_client *client);
int block_client_identify(struct block_client *client);
int block_client_read_one(struct block_client *client, u64 block_index, u8 *out, u64 out_len);

#endif
