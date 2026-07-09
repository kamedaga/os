#include "client.h"
#include "../lpr_error.h"
#include "../support/string.h"
#include "../support/syscall.h"

#include <lpr_supervisor/ipc_protocol_v2.h>
#include <pacha/ipc.h>
#include <pacha/service_abi.h>
#include <pachaos/abi.h>
#include <personality/linux_lpr.h>
#include <stdint.h>

void *lpr_process_client_payload(void *page)
{
    return page == 0 ? 0 : (void *)((uint8_t *)page + PACHA_SERVICE_HEADER_BYTES);
}

int64_t lpr_process_client_call(
    uint64_t *request_counter,
    int64_t (*status_to_errno)(int64_t status),
    uint32_t op,
    int page_fd,
    void *page,
    uint32_t payload_size,
    int transfer_fd,
    uint64_t *out_result)
{
    if (request_counter == 0 ||
        status_to_errno == 0 ||
        page_fd < 16 ||
        page == 0 ||
        payload_size > LPRS_V2_PAYLOAD_BYTES)
    {
        return -LPR_LINUX_EINVAL;
    }

    struct pacha_ipc_fd fds[2];
    uint64_t fd_count = 0;
    const uint64_t request_id = ++*request_counter;
    pacha_service_request_header_t *header = (pacha_service_request_header_t *)page;
    lpr_memset(header, 0, sizeof(*header));
    header->magic = PACHA_SERVICE_REQUEST_MAGIC;
    header->abi_version = PACHA_SERVICE_ABI_VERSION;
    header->service_id = LPRS_V2_SERVICE_ID;
    header->op = op;
    header->flags = payload_size != 0 ? PACHA_SERVICE_FLAG_PAGE_PAYLOAD : 0;
    header->request_id = request_id;
    header->trace_id = request_id;
    header->payload_size = payload_size;
    header->fd_count = transfer_fd >= 16 ? 1u : 0u;

    lpr_memset(fds, 0, sizeof(fds));
    fds[fd_count].fd = (uint64_t)(uint32_t)page_fd;
    fds[fd_count].rights =
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    fd_count++;
    if (transfer_fd >= 16) {
        fds[fd_count].fd = (uint64_t)(uint32_t)transfer_fd;
        fds[fd_count].rights =
            PACHA_FD_RIGHT_INSPECT |
            PACHA_FD_RIGHT_WAIT |
            PACHA_FD_RIGHT_POLL |
            PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_KILL;
        fd_count++;
    }

    const struct pacha_ipc_msg request = {
        .word0 = PACHA_SERVICE_REQUEST_MAGIC,
        .word1 = 0,
        .word2 = 0,
        .word3 = request_id,
        .fds = fds,
        .fd_count = fd_count,
    };
    const int64_t reply_fd = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_IPC_CALL,
        LPR_SUPERVISOR_ENDPOINT_FD,
        (uint64_t)(uintptr_t)&request);
    if (reply_fd < 16) {
        const int64_t err = status_to_errno(reply_fd);
        lpr_trace_error_record(
            LPR_ERROR_DOMAIN_KERNEL,
            op,
            LPR_ERROR_STAGE_CHILD_RPC_CALL,
            err,
            reply_fd,
            request_id,
            fd_count,
            LPR_SUPERVISOR_ENDPOINT_FD,
            0,
            "lpr process supervisor ipc_call failed");
        return err;
    }

    struct pacha_ipc_msg reply;
    lpr_memset(&reply, 0, sizeof(reply));
    const int64_t recv_status = lpr_pacha_syscall4(
        PACHAOS_SYSCALL_IPC_RECV_WAIT,
        (uint64_t)(uint32_t)reply_fd,
        (uint64_t)(uintptr_t)&reply,
        UINT64_MAX,
        0);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)reply_fd);
    if (recv_status != 0) {
        const int64_t err = status_to_errno(recv_status);
        lpr_trace_error_record(
            LPR_ERROR_DOMAIN_KERNEL,
            op,
            LPR_ERROR_STAGE_CHILD_RPC_RECV,
            err,
            recv_status,
            request_id,
            fd_count,
            (uint64_t)(uint32_t)reply_fd,
            0,
            "lpr process supervisor reply recv failed");
        return err;
    }

    const pacha_service_reply_header_t *reply_header = (const pacha_service_reply_header_t *)page;
    if (reply.word0 != PACHA_SERVICE_REPLY_MAGIC ||
        reply.word3 != request_id ||
        reply_header->magic != PACHA_SERVICE_REPLY_MAGIC ||
        reply_header->request_id != request_id)
    {
        lpr_trace_error_record(
            LPR_ERROR_DOMAIN_LPRS,
            op,
            LPR_ERROR_STAGE_REPLY_MAGIC,
            -LPR_LINUX_EIO,
            (int64_t)reply.word0,
            request_id,
            fd_count,
            reply.word3,
            reply.word2,
            "lpr process supervisor reply mismatch");
        return -LPR_LINUX_EIO;
    }
    if ((int64_t)reply.word1 < 0) {
        lpr_trace_error_record(
            LPR_ERROR_DOMAIN_LPRS,
            op,
            LPR_ERROR_STAGE_CHILD_STATUS,
            (int64_t)reply.word1,
            (int64_t)reply.word1,
            request_id,
            fd_count,
            0,
            reply.word2,
            "lpr process supervisor returned error");
        return (int64_t)reply.word1;
    }
    if (out_result != 0) {
        *out_result = reply.word2;
    }
    return 0;
}

int64_t lpr_process_client_call_token(
    uint64_t *request_counter,
    int64_t (*status_to_errno)(int64_t status),
    int (*create_page)(void **out_page),
    void (*destroy_page)(int fd, void *page),
    uint32_t op,
    uint64_t token,
    int transfer_fd,
    uint64_t *out_result)
{
    if (create_page == 0 || destroy_page == 0 || token == 0) {
        return -LPR_LINUX_EINVAL;
    }
    void *page = 0;
    const int page_fd = create_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    lpr_memset(page, 0, PACHA_SERVICE_PAGE_BYTES);
    lprs_v2_token_request_t *req = (lprs_v2_token_request_t *)lpr_process_client_payload(page);
    req->token = token;
    const int64_t status = lpr_process_client_call(
        request_counter,
        status_to_errno,
        op,
        page_fd,
        page,
        sizeof(*req),
        transfer_fd,
        out_result);
    destroy_page(page_fd, page);
    return status;
}
