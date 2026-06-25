#include "control_endpoint.h"

#include "ipc_wire.h"
#include "koboxd/ipc_protocol.h"

#include <stdio.h>

int koboxd_control_serve_get_endpoint(
    koboxd_ipc_service_t *ipc_service,
    int control_fd,
    uint64_t expected_kind)
{
    struct pacha_ipc_msg request = {0};
    int status = koboxd_recv_ipc_wait(control_fd, &request);
    if (status != 0) {
        fprintf(stderr, "[koboxd] control recv failed status=%d\n", status);
        return status;
    }
    if (request.word0 != KOBOXD_WIRE_CONTROL_MAGIC ||
        request.word1 != KOBOXD_WIRE_CONTROL_GET_ENDPOINT ||
        request.word2 != expected_kind ||
        request.word3 != KOBOXD_WIRE_VERSION)
    {
        fprintf(stderr,
            "[koboxd] control request invalid word0=0x%llx op=%llu kind=%llu version=%llu\n",
            (unsigned long long)request.word0,
            (unsigned long long)request.word1,
            (unsigned long long)request.word2,
            (unsigned long long)request.word3);
        return -2;
    }

    struct pacha_ipc_channel_pair pair;
    status = koboxd_create_service_channel_pair(&pair);
    if (status != 0) {
        fprintf(stderr, "[koboxd] service channel create failed status=%d\n", status);
        return status;
    }
    koboxd_ipc_endpoint_t *endpoint =
        koboxd_ipc_service_endpoint(ipc_service, (koboxd_ipc_endpoint_kind_t)expected_kind);
    if (endpoint == NULL) {
        return -3;
    }
    endpoint->endpoint_fd = pair.b;
    endpoint->ready = 1;

    status = koboxd_send_endpoint_fd(control_fd, request.word3, expected_kind, pair.a);
    if (status != 0) {
        fprintf(stderr,
            "[koboxd] endpoint fd send failed kind=%llu status=%d\n",
            (unsigned long long)expected_kind,
            status);
        return status;
    }
    return 0;
}

int koboxd_block_serve_identify(
    koboxd_ipc_service_t *ipc_service,
    const koboxd_block_service_t *block_service)
{
    const koboxd_ipc_endpoint_t *endpoint =
        koboxd_ipc_service_endpoint_const(ipc_service, KOBOXD_IPC_ENDPOINT_BLOCK);
    if (endpoint == NULL || endpoint->endpoint_fd < 16 || block_service == NULL) {
        return -1;
    }
    struct pacha_ipc_msg request = {0};
    int status = koboxd_recv_ipc_wait(endpoint->endpoint_fd, &request);
    if (status != 0) {
        return status;
    }
    if (request.word0 != KOBOXD_WIRE_ENDPOINT_MAGIC || request.word1 != KOBOXD_WIRE_BLOCK_IDENTIFY) {
        return -2;
    }
    return koboxd_send_status_reply(
        endpoint->endpoint_fd,
        request.word3,
        block_service->logical_block_size);
}
