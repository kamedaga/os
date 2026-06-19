#pragma once

#include "ipc_service.h"

#include <stddef.h>
#include <stdint.h>

typedef struct koboxd_block_service {
    void *disk;
    uint64_t logical_block_size;
} koboxd_block_service_t;

typedef struct koboxd_block_read_reply {
    uint8_t sample[64];
} koboxd_block_read_reply_t;

int koboxd_block_service_init(koboxd_block_service_t *service, void *disk);
int koboxd_block_service_handle_ipc(void *ctx, const koboxd_ipc_request_t *request, koboxd_ipc_reply_t *reply);
