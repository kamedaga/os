#include "block_service.h"

#include "linux_subsystem/block/block.h"

#include <stdio.h>
#include <string.h>

int koboxd_block_service_init(koboxd_block_service_t *service, void *disk)
{
    if (service == NULL || disk == NULL) {
        return -1;
    }
    memset(service, 0, sizeof(*service));
    service->disk = disk;
    service->logical_block_size = 512;
    return 0;
}

int koboxd_block_service_handle_ipc(void *ctx, const koboxd_ipc_request_t *request, koboxd_ipc_reply_t *reply)
{
    koboxd_block_service_t *service = (koboxd_block_service_t *)ctx;
    if (service == NULL || service->disk == NULL || request == NULL || reply == NULL) {
        return -1;
    }
    int status = koboxd_ipc_validate_request(request, KOBOXD_IPC_ENDPOINT_BLOCK);
    if (status != 0) {
        return status;
    }

    switch (request->header.op) {
    case KOBOXD_IPC_BLOCK_OP_IDENTIFY:
        return koboxd_ipc_make_reply(
            request,
            0,
            service->logical_block_size,
            0,
            NULL,
            0,
            reply);
    case KOBOXD_IPC_BLOCK_OP_READ: {
        uint8_t sector[512];
        memset(sector, 0, sizeof(sector));
        uint64_t length = request->header.length;
        if (length == 0 || length > sizeof(sector)) {
            length = sizeof(sector);
        }
        status = kb_block_subsystem_disk_read(service->disk, request->header.offset, sector, (size_t)length);
        if (status != 0) {
            return koboxd_ipc_make_reply(request, status, 0, 0, NULL, 0, reply);
        }
        koboxd_block_read_reply_t payload;
        memset(&payload, 0, sizeof(payload));
        memcpy(payload.sample, sector, length < sizeof(payload.sample) ? (size_t)length : sizeof(payload.sample));
        return koboxd_ipc_make_reply(
            request,
            0,
            length,
            service->logical_block_size,
            &payload,
            sizeof(payload),
            reply);
    }
    default:
        return koboxd_ipc_make_reply(request, -95, 0, 0, NULL, 0, reply);
    }
}
