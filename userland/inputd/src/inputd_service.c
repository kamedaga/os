#include "inputd_service.h"

#include <pacha/abi.h>
#include <pacha/service_abi.h>
#include <netd/ipc_protocol.h>

#include <stdint.h>
#include <string.h>

_Static_assert(INPUTD_INPUT_CAP_KEYBOARD == NETD_INPUT_CAP_KEYBOARD,
    "input keyboard capability ABI");
_Static_assert(INPUTD_INPUT_CAP_RELATIVE == NETD_INPUT_CAP_RELATIVE,
    "input relative capability ABI");
_Static_assert(INPUTD_INPUT_CAP_ABSOLUTE == NETD_INPUT_CAP_ABSOLUTE,
    "input absolute capability ABI");

static int publish_device(
    inputd_service_t *service,
    const struct inputd_public_device *input_device)
{
    static uint64_t request_id;
    if (service == NULL || service->cfg == NULL || input_device == NULL ||
        service->cfg->netd_endpoint_fd < 16 ||
        input_device->event_index > UINT16_MAX ||
        input_device->capabilities > UINT8_MAX ||
        input_device->pci_segment > UINT16_MAX || input_device->pci_bus > UINT8_MAX ||
        input_device->pci_device > 31 || input_device->pci_function > 7)
        return -22;
    const uint64_t device = netd_input_uevent_encode((netd_input_uevent_descriptor_t){
        .segment = (uint16_t)input_device->pci_segment,
        .bus = (uint8_t)input_device->pci_bus,
        .device = (uint8_t)input_device->pci_device,
        .function = (uint8_t)input_device->pci_function,
        .capabilities = (uint8_t)input_device->capabilities,
        .event_index = (uint16_t)input_device->event_index,
    });
    const struct pacha_ipc_msg request = {
        .word0 = PACHA_SERVICE_REQUEST_MAGIC,
        .word1 = NETD_OP_UEVENT_PUBLISH,
        .word2 = device,
        .word3 = ++request_id,
    };
    const int reply_fd = pacha_ipc_call((int)service->cfg->netd_endpoint_fd, &request);
    if (reply_fd < 16) return reply_fd;
    struct pacha_ipc_msg reply;
    memset(&reply, 0, sizeof(reply));
    const int recv_status = pacha_ipc_recv_wait(reply_fd, &reply, PACHA_FD_WAIT_FOREVER);
    (void)pacha_fd_close(reply_fd);
    if (recv_status != 0) return recv_status;
    if (reply.word0 != PACHA_SERVICE_REPLY_MAGIC || reply.word3 != request.word3) return -5;
    return (int)(int64_t)reply.word1;
}

int inputd_service_publish_startup_devices(inputd_service_t *service)
{
    if (service == NULL || service->input == NULL) return -22;
    const size_t count = inputd_input_public_device_count(service->input);
    if (count == 0) return -19;
    for (size_t i = 0; i < count; i++) {
        struct inputd_public_device device;
        int status = inputd_input_public_device(service->input, i, &device);
        if (status == 0) status = publish_device(service, &device);
        if (status != 0) return status;
    }
    return 0;
}

static void close_received(
    const struct pacha_ipc_msg *request,
    const struct pacha_ipc_fd *fds,
    int reply_fd,
    int keep_fd)
{
    for (uint64_t i = 0; i < request->fd_count; i++) {
        const int fd = (int)(uint32_t)fds[i].fd;
        if (fd >= 16 && fd != reply_fd && fd != keep_fd) (void)pacha_fd_close(fd);
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
        close_received(request, fds, reply_fd, -1);
        return -22;
    }
    void *page = pacha_mmap(page_fd, PACHA_SERVICE_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (page == NULL) {
        close_received(request, fds, reply_fd, -1);
        return -5;
    }
    pacha_service_envelope_t header;
    memcpy(&header, page, sizeof(header));
    int64_t status = -22;
    uint64_t result = 0;
    int keep_fd = -1;
    if (request->word0 == PACHA_SERVICE_REQUEST_MAGIC &&
        request->word3 == header.request_id &&
        pacha_service_request_is_valid(&header, INPUTD_SERVICE_ID)) {
        void *payload = (uint8_t *)page + PACHA_SERVICE_HEADER_BYTES;
        switch (header.op) {
        case INPUTD_OP_OPEN:
            if (header.payload_size >= sizeof(inputd_open_request_t)) {
                const inputd_open_request_t *open = payload;
                const int notify_fd = request->fd_count >= 3 ? (int)(uint32_t)fds[1].fd : -1;
                status = inputd_input_open(open->event_index, open->flags, notify_fd, &result);
                if (status == 0) keep_fd = notify_fd;
            } else {
                status = -22;
            }
            break;
        case INPUTD_OP_CLOSE:
            status = header.payload_size >= sizeof(inputd_handle_request_t) ?
                inputd_input_close(((inputd_handle_request_t *)payload)->handle) : -22;
            break;
        case INPUTD_OP_DUP:
            if (header.payload_size >= sizeof(inputd_handle_request_t)) {
                const int notify_fd = request->fd_count >= 3 ?
                    (int)(uint32_t)fds[1].fd : -1;
                status = notify_fd >= 16 ? inputd_input_transfer_dup(
                    ((inputd_handle_request_t *)payload)->handle, notify_fd, &result) :
                    inputd_input_dup(
                        ((inputd_handle_request_t *)payload)->handle, &result);
                if (status == 0 && notify_fd >= 16) keep_fd = notify_fd;
            } else {
                status = -22;
            }
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
    close_received(request, fds, reply_fd, keep_fd);
    const int reply_status = send_reply(reply_fd, page, &header, status, result);
    (void)pacha_munmap(page, PACHA_SERVICE_PAGE_BYTES);
    return reply_status;
}
