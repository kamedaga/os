#include "drmd_service.h"

#include <pacha/abi.h>
#include <pacha/service_abi.h>

#include <stdint.h>
#include <string.h>

static void close_received(
    const struct pacha_ipc_msg *request,
    const struct pacha_ipc_fd *fds,
    int reply_fd,
    int keep_fd)
{
    for (uint64_t i = 0; i < request->fd_count; i++) {
        const int fd = (int)(uint32_t)fds[i].fd;
        if (fd >= 16 && fd != reply_fd && fd != keep_fd) {
            (void)pacha_fd_close(fd);
        }
    }
}

static int send_reply(
    int reply_fd,
    void *page,
    const pacha_service_envelope_t *request_header,
    int64_t status,
    uint64_t result,
    int transfer_fd,
    uint64_t transfer_rights,
    uint64_t transfer_flags)
{
    pacha_service_reply_init(
        (pacha_service_envelope_t *)page,
        request_header,
        status,
        status < 0 ? PACHA_SERVICE_ERROR_DRMD_DRM : PACHA_SERVICE_ERROR_NONE,
        status < 0 ? 0 : result,
        0);
    struct pacha_ipc_fd fd;
    memset(&fd, 0, sizeof(fd));
    if (status == 0 && transfer_fd >= 16) {
        fd.fd = (uint64_t)(uint32_t)transfer_fd;
        fd.rights = transfer_rights;
        fd.transfer_flags = transfer_flags;
    }
    const struct pacha_ipc_msg reply = {
        .word0 = PACHA_SERVICE_REPLY_MAGIC,
        .word1 = (uint64_t)status,
        .word2 = status < 0 ? 0 : result,
        .word3 = request_header->request_id,
        .fds = status == 0 && transfer_fd >= 16 ? &fd : NULL,
        .fd_count = status == 0 && transfer_fd >= 16 ? 1u : 0u,
    };
    const int reply_status = pacha_ipc_reply(reply_fd, &reply);
    (void)pacha_fd_close(reply_fd);
    return reply_status;
}

int drmd_service_send_boot_ready(drmd_service_t *service, int64_t status, uint64_t result)
{
    if (service == NULL || service->cfg == NULL || service->cfg->ready_channel_fd < 16) {
        return -22;
    }
    const struct pacha_ipc_msg ready = {
        .word0 = DRMD_BOOT_READY_MAGIC,
        .word1 = (uint64_t)status,
        .word2 = status == 0 ? result : 0,
        .word3 = 0,
    };
    return pacha_ipc_send((int)service->cfg->ready_channel_fd, &ready);
}

int drmd_service_dispatch(
    drmd_service_t *service,
    const struct pacha_ipc_msg *request,
    const struct pacha_ipc_fd *fds)
{
    if (service == NULL || request == NULL || fds == NULL || request->fd_count < 2) {
        return -22;
    }
    const int reply_fd = (int)(uint32_t)fds[request->fd_count - 1u].fd;
    const int page_fd = (int)(uint32_t)fds[0].fd;
    if (reply_fd < 16 || page_fd < 16 || page_fd == reply_fd) {
        close_received(request, fds, reply_fd, -1);
        return -22;
    }
    void *page = pacha_mmap(
        page_fd,
        DRMD_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
    if (page == NULL) {
        close_received(request, fds, reply_fd, -1);
        return -5;
    }
    pacha_service_envelope_t header;
    memcpy(&header, page, sizeof(header));
    int64_t status = -22;
    uint64_t result = 0;
    int transfer_fd = -1;
    uint64_t transfer_rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    uint64_t transfer_flags = 0;
    int prime_export_ref = 0;
    int imported_vmo_fd = -1;
    if (request->word0 == PACHA_SERVICE_REQUEST_MAGIC &&
        request->word3 == header.request_id &&
        pacha_service_request_is_valid(&header, DRMD_SERVICE_ID)) {
        void *payload = (uint8_t *)page + PACHA_SERVICE_HEADER_BYTES;
        switch (header.op) {
        case DRMD_OP_HELLO:
            status = service->drm != NULL && service->drm->ready ? 0 : -19;
            result = status == 0 ? 1 : 0;
            break;
        case DRMD_OP_OPEN_CARD:
            status = header.payload_size >= sizeof(drmd_open_request_t) ?
                drmd_drm_island_open(service->drm, payload, &result) : -22;
            break;
        case DRMD_OP_HANDLE_CLOSE:
            status = header.payload_size >= sizeof(drmd_handle_request_t) ?
                drmd_drm_island_close(service->drm, ((drmd_handle_request_t *)payload)->handle) : -22;
            break;
        case DRMD_OP_HANDLE_DUP:
            status = header.payload_size >= sizeof(drmd_handle_request_t) ?
                drmd_drm_island_dup(service->drm, ((drmd_handle_request_t *)payload)->handle, &result) : -22;
            break;
        case DRMD_OP_HANDLE_IOCTL:
            status = header.payload_size >= sizeof(drmd_ioctl_request_t) ?
                drmd_drm_island_ioctl(service->drm, payload) : -22;
            break;
        case DRMD_OP_HANDLE_MMAP:
            status = header.payload_size >= sizeof(drmd_mmap_request_t) ?
                drmd_drm_island_mmap(service->drm, payload, &transfer_fd) : -22;
            break;
        case DRMD_OP_HANDLE_READ:
            status = header.payload_size >= sizeof(drmd_read_request_t) ?
                drmd_drm_island_read(service->drm, payload, &result) : -22;
            break;
        case DRMD_OP_HANDLE_POLL:
            status = header.payload_size >= sizeof(drmd_handle_request_t) ?
                drmd_drm_island_poll(service->drm, payload, &result) : -22;
            break;
        case DRMD_OP_PRIME_EXPORT:
            status = header.payload_size >= sizeof(drmd_prime_export_request_t) ?
                drmd_drm_island_prime_export(
                    service->drm, payload, &result, &transfer_fd, &transfer_rights) : -22;
            if (status == 0) {
                const drmd_prime_export_request_t *prime = payload;
                transfer_flags = (prime->flags & DRMD_CLOEXEC) != 0 ?
                    PACHA_IPC_TRANSFER_CLOEXEC : 0;
                prime_export_ref = 1;
            }
            break;
        case DRMD_OP_PRIME_IMPORT:
            if (header.payload_size >= sizeof(drmd_prime_import_request_t)) {
                const drmd_prime_import_request_t *prime = payload;
                const int candidate_vmo_fd = request->fd_count >= 3 ? (int)(uint32_t)fds[1].fd : -1;
                status = drmd_drm_island_prime_import(
                    service->drm, prime, candidate_vmo_fd, &result);
                if (status == 0 && prime->token == 0) imported_vmo_fd = candidate_vmo_fd;
            }
            break;
        case DRMD_OP_PRIME_RELEASE:
            status = header.payload_size >= sizeof(drmd_prime_token_request_t) ?
                drmd_drm_island_prime_release(
                    service->drm, ((drmd_prime_token_request_t *)payload)->token) : -22;
            break;
        case DRMD_OP_PRIME_ACQUIRE:
            status = header.payload_size >= sizeof(drmd_prime_token_request_t) ?
                drmd_drm_island_prime_acquire(
                    service->drm, ((drmd_prime_token_request_t *)payload)->token) : -22;
            break;
        default:
            status = -95;
            break;
        }
    }
    close_received(request, fds, reply_fd, imported_vmo_fd);
    const int reply_status = send_reply(
        reply_fd, page, &header, status, result, transfer_fd, transfer_rights, transfer_flags);
    if (reply_status != 0 && prime_export_ref) {
        (void)drmd_drm_island_prime_release(service->drm, result);
    }
    (void)pacha_munmap(page, DRMD_PAGE_BYTES);
    return reply_status;
}
