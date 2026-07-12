#include "inputd_service.h"

#include <pacha/abi.h>
#include <pacha/service_abi.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void close_received(
    const struct pacha_ipc_msg *request,
    const struct pacha_ipc_fd *fds,
    int reply_fd)
{
    for (uint64_t i = 0; i < request->fd_count; i++) {
        const int fd = (int)(uint32_t)fds[i].fd;
        if (fd >= 16 && fd != reply_fd) (void)pacha_fd_close(fd);
    }
}

static int send_reply(
    int reply_fd,
    void *page,
    const pacha_service_envelope_t *request_header,
    int64_t status,
    uint64_t result)
{
    pacha_service_reply_init(
        (pacha_service_envelope_t *)page,
        request_header,
        status,
        status < 0 ? PACHA_SERVICE_ERROR_INPUTD_INPUT : PACHA_SERVICE_ERROR_NONE,
        status < 0 ? 0 : result,
        0);
    const struct pacha_ipc_msg reply = {
        .word0 = PACHA_SERVICE_REPLY_MAGIC,
        .word1 = (uint64_t)status,
        .word2 = status < 0 ? 0 : result,
        .word3 = request_header->request_id,
    };
    const int reply_status = pacha_ipc_reply(reply_fd, &reply);
    (void)pacha_fd_close(reply_fd);
    return reply_status;
}

int inputd_service_send_boot_ready(inputd_service_t *service, int64_t status, uint64_t result)
{
    if (service == NULL || service->cfg == NULL || service->cfg->ready_channel_fd < 16)
        return -22;
    const struct pacha_ipc_msg ready = {
        .word0 = INPUTD_BOOT_READY_MAGIC,
        .word1 = (uint64_t)status,
        .word2 = status == 0 ? result : 0,
    };
    return pacha_ipc_send((int)service->cfg->ready_channel_fd, &ready);
}

int inputd_service_dispatch(
    inputd_service_t *service,
    const struct pacha_ipc_msg *request,
    const struct pacha_ipc_fd *fds)
{
    if (service == NULL || request == NULL || fds == NULL || request->fd_count < 2)
        return -22;
    const int reply_fd = (int)(uint32_t)fds[request->fd_count - 1u].fd;
    const int page_fd = (int)(uint32_t)fds[0].fd;
    if (reply_fd < 16 || page_fd < 16 || page_fd == reply_fd) {
        close_received(request, fds, reply_fd);
        return -22;
    }
    void *page = pacha_mmap(page_fd, PACHA_SERVICE_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (page == NULL) {
        close_received(request, fds, reply_fd);
        return -5;
    }
    pacha_service_envelope_t header;
    memcpy(&header, page, sizeof(header));
    int64_t status = -22;
    uint64_t result = 0;
    inputd_input_island_pump(service->input);
    if (request->word0 == PACHA_SERVICE_REQUEST_MAGIC &&
        request->word3 == header.request_id &&
        pacha_service_request_is_valid(&header, INPUTD_SERVICE_ID)) {
        void *payload = (uint8_t *)page + PACHA_SERVICE_HEADER_BYTES;
        switch (header.op) {
        case INPUTD_OP_OPEN:
            status = header.payload_size >= sizeof(inputd_open_request_t) ?
                inputd_input_open(((inputd_open_request_t *)payload)->event_index,
                    ((inputd_open_request_t *)payload)->flags, &result) : -22;
            break;
        case INPUTD_OP_CLOSE:
            status = header.payload_size >= sizeof(inputd_handle_request_t) ?
                inputd_input_close(((inputd_handle_request_t *)payload)->handle) : -22;
            break;
        case INPUTD_OP_DUP:
            status = header.payload_size >= sizeof(inputd_handle_request_t) ?
                inputd_input_dup(((inputd_handle_request_t *)payload)->handle, &result) : -22;
            break;
        case INPUTD_OP_READ:
            status = header.payload_size >= sizeof(inputd_read_request_t) ?
                inputd_input_read(payload) : -22;
            if (status == 0) result = ((inputd_read_request_t *)payload)->event_count;
            break;
        case INPUTD_OP_IOCTL:
            status = header.payload_size >= sizeof(inputd_ioctl_request_t) ?
                inputd_input_ioctl(payload) : -22;
            if (header.payload_size >= sizeof(inputd_ioctl_request_t)) {
                const inputd_ioctl_request_t *ioctl_request = payload;
                printf("[inputd] ioctl handle=%llu request=0x%08llx size=%u status=%lld result=%u\n",
                    (unsigned long long)ioctl_request->handle,
                    (unsigned long long)ioctl_request->request,
                    ioctl_request->data_size,
                    (long long)status,
                    ioctl_request->result_size);
                if (status == 0) result = ioctl_request->result_size;
            }
            break;
        case INPUTD_OP_POLL:
            status = header.payload_size >= sizeof(inputd_poll_request_t) ?
                inputd_input_poll(payload) : -22;
            if (status == 0) result = ((inputd_poll_request_t *)payload)->revents;
            break;
        default:
            status = -95;
            break;
        }
    }
    close_received(request, fds, reply_fd);
    const int reply_status = send_reply(reply_fd, page, &header, status, result);
    (void)pacha_munmap(page, PACHA_SERVICE_PAGE_BYTES);
    return reply_status;
}
