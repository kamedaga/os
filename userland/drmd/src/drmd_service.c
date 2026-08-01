#include "drmd_service.h"
#include "drm_kms.h"

#include <pacha/abi.h>
#include <pacha/service_abi.h>

#include <stdint.h>
#include <string.h>

static void close_received(
    const struct pacha_ipc_msg *request,
    const struct pacha_ipc_fd *fds,
    const int *keep_fds,
    size_t keep_count)
{
    for (uint64_t i = 0; i < request->fd_count; i++) {
        const int fd = (int)(uint32_t)fds[i].fd;
        int keep = 0;
        for (size_t j = 0; j < keep_count; j++) {
            if (fd == keep_fds[j]) {
                keep = 1;
                break;
            }
        }
        if (fd >= 16 && !keep) {
            (void)pacha_fd_close(fd);
        }
    }
}

static int valid_channel_fd(int fd, uint64_t required_rights)
{
    struct pacha_fd_info info;
    memset(&info, 0, sizeof(info));
    return fd >= 16 && pacha_fd_get_info(fd, &info) == 0 &&
        info.kind == PACHA_FD_KIND_CHANNEL &&
        (info.rights & required_rights) == required_rights;
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
        const int keep[] = { reply_fd };
        close_received(request, fds, keep, 1);
        return -22;
    }
    void *page = pacha_mmap(
        page_fd,
        DRMD_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
    if (page == NULL) {
        const int keep[] = { reply_fd };
        close_received(request, fds, keep, 1);
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
    int retained_received_fd = -1;
    drmd_ioctl_attachments_t ioctl_attachments = {
        .input_wait_fd = -1,
        .output_notify_fd = -1,
    };
    void *ioctl_aux_mapping = NULL;
    uint64_t ioctl_aux_mapping_size = 0;
    int ioctl_aux_fd = -1;
    if (request->word0 == PACHA_SERVICE_REQUEST_MAGIC &&
        request->word3 == header.request_id &&
        pacha_service_request_is_valid(&header, DRMD_SERVICE_ID)) {
        void *payload = (uint8_t *)page + PACHA_SERVICE_HEADER_BYTES;
        switch (header.op) {
        case DRMD_OP_HELLO:
            status = service->drm != NULL && service->drm->ready ? 0 : -19;
            result = status == 0 ? 1 : 0;
            break;
        case DRMD_OP_OPEN_NODE:
            {
            const int notify_fd = request->fd_count >= 3 ? (int)(uint32_t)fds[1].fd : -1;
            status = header.payload_size >= sizeof(drmd_open_request_t) ?
                drmd_drm_island_open(service->drm, payload, notify_fd, &result) : -22;
            if (status == 0) retained_received_fd = notify_fd;
            break;
            }
        case DRMD_OP_HANDLE_CLOSE:
            status = header.payload_size >= sizeof(drmd_handle_request_t) ?
                drmd_drm_island_close(service->drm, ((drmd_handle_request_t *)payload)->handle) : -22;
            break;
        case DRMD_OP_HANDLE_DUP:
            if (header.payload_size >= sizeof(drmd_handle_request_t)) {
                const int lease_fd = request->fd_count >= 3 ?
                    (int)(uint32_t)fds[1].fd : -1;
                status = lease_fd >= 16 ? drmd_drm_island_transfer_dup(
                    service->drm,
                    ((drmd_handle_request_t *)payload)->handle,
                    lease_fd,
                    &result) : drmd_drm_island_dup(
                        service->drm,
                        ((drmd_handle_request_t *)payload)->handle,
                        &result);
                if (status == 0 && lease_fd >= 16) retained_received_fd = lease_fd;
            } else {
                status = -22;
            }
            break;
        case DRMD_OP_HANDLE_IOCTL:
            if (header.payload_size >= sizeof(drmd_ioctl_request_t)) {
                drmd_ioctl_request_t *ioctl = payload;
                uint64_t fd_index = 1;
                int descriptor_status =
                    ioctl->reserved0 == 0 &&
                    (ioctl->fd_flags & ~DRMD_IOCTL_FD_MASK) == 0 ? 0 : -22;
                if (descriptor_status == 0 && ioctl->aux_size != 0) {
                    const int aux_fd = fd_index + 1u < request->fd_count ?
                        (int)(uint32_t)fds[fd_index].fd : -1;
                    ioctl_aux_fd = aux_fd;
                    fd_index++;
                    struct pacha_fd_info info;
                    memset(&info, 0, sizeof(info));
                    ioctl_aux_mapping_size =
                        (ioctl->aux_size + 4095u) & ~UINT64_C(4095);
                    if (ioctl->aux_size <= DRMD_IOCTL_AUX_MAX_BYTES &&
                        aux_fd >= 16 && aux_fd != page_fd && aux_fd != reply_fd &&
                        ioctl_aux_mapping_size >= ioctl->aux_size &&
                        pacha_fd_get_info(aux_fd, &info) == 0 &&
                        info.kind == PACHA_FD_KIND_VMO &&
                        info.size >= ioctl_aux_mapping_size &&
                        (info.rights & (PACHA_FD_RIGHT_CLOSE |
                            PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE)) ==
                            (PACHA_FD_RIGHT_CLOSE |
                                PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE)) {
                        ioctl_aux_mapping = pacha_mmap(
                            aux_fd,
                            ioctl_aux_mapping_size,
                            PACHA_PROT_READ | PACHA_PROT_WRITE,
                            PACHA_MMAP_SHARED,
                            0);
                    }
                    if (ioctl_aux_mapping == NULL) descriptor_status = -22;
                }
                if (descriptor_status == 0 &&
                    (ioctl->fd_flags & DRMD_IOCTL_FD_INPUT_WAIT) != 0) {
                    ioctl_attachments.input_wait_fd =
                        fd_index + 1u < request->fd_count ?
                            (int)(uint32_t)fds[fd_index].fd : -1;
                    fd_index++;
                    if (!valid_channel_fd(
                            ioctl_attachments.input_wait_fd,
                            PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL |
                                PACHA_FD_RIGHT_CLOSE)) {
                        descriptor_status = -22;
                    }
                }
                if (descriptor_status == 0 &&
                    (ioctl->fd_flags & DRMD_IOCTL_FD_OUTPUT_NOTIFY) != 0) {
                    ioctl_attachments.output_notify_fd =
                        fd_index + 1u < request->fd_count ?
                            (int)(uint32_t)fds[fd_index].fd : -1;
                    fd_index++;
                    if (!valid_channel_fd(
                            ioctl_attachments.output_notify_fd,
                            PACHA_FD_RIGHT_SEND | PACHA_FD_RIGHT_CLOSE)) {
                        descriptor_status = -22;
                    }
                }
                if (descriptor_status == 0 &&
                    (fd_index + 1u != request->fd_count ||
                        (int)(uint32_t)fds[fd_index].fd != reply_fd ||
                        (((ioctl->fd_flags & DRMD_IOCTL_FD_MASK) ==
                            DRMD_IOCTL_FD_MASK) &&
                            ioctl_attachments.input_wait_fd ==
                                ioctl_attachments.output_notify_fd) ||
                        ioctl_attachments.input_wait_fd == page_fd ||
                        ioctl_attachments.output_notify_fd == page_fd ||
                        ioctl_attachments.input_wait_fd == reply_fd ||
                        ioctl_attachments.output_notify_fd == reply_fd ||
                        (ioctl_aux_fd >= 16 &&
                            (ioctl_aux_fd == ioctl_attachments.input_wait_fd ||
                                ioctl_aux_fd == ioctl_attachments.output_notify_fd)))) {
                    descriptor_status = -22;
                }
                status = descriptor_status == 0 ?
                    drmd_drm_island_ioctl(
                        service->drm,
                        ioctl,
                        ioctl_aux_mapping,
                        ioctl->aux_size,
                        &ioctl_attachments) : descriptor_status;
            } else {
                status = -22;
            }
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
                if (status == 0 && prime->token == 0) retained_received_fd = candidate_vmo_fd;
            }
            break;
        case DRMD_OP_PRIME_IMPORT_SYNC_FILE:
            if (header.payload_size == sizeof(drmd_prime_token_request_t) &&
                request->fd_count == 3)
            {
                const int wait_fd = (int)(uint32_t)fds[1].fd;
                status = wait_fd >= 16 && wait_fd != page_fd && wait_fd != reply_fd ?
                    drmd_kms_prime_import_sync_file(
                        ((drmd_prime_token_request_t *)payload)->token,
                        wait_fd) : -22;
                if (status == 0) retained_received_fd = wait_fd;
            } else {
                status = -22;
            }
            break;
        case DRMD_OP_PRIME_RELEASE:
            status = header.payload_size >= sizeof(drmd_prime_token_request_t) ?
                drmd_drm_island_prime_release(
                    service->drm, ((drmd_prime_token_request_t *)payload)->token) : -22;
            break;
        case DRMD_OP_PRIME_ACQUIRE:
            if (header.payload_size >= sizeof(drmd_prime_token_request_t) &&
                (request->fd_count == 2 ||
                 (request->fd_count == 3 && fds[1].fd >= 16)))
            {
                const int lease_fd = request->fd_count == 3 ?
                    (int)(uint32_t)fds[1].fd : -1;
                status = drmd_drm_island_prime_acquire(
                    service->drm,
                    ((drmd_prime_token_request_t *)payload)->token,
                    lease_fd);
                if (status == 0 && lease_fd >= 16) retained_received_fd = lease_fd;
            } else {
                status = -22;
            }
            break;
        default:
            status = -95;
            break;
        }
    }
    if (ioctl_aux_mapping != NULL) {
        (void)pacha_munmap(ioctl_aux_mapping, ioctl_aux_mapping_size);
        ioctl_aux_mapping = NULL;
    }
    int keep_fds[5];
    size_t keep_count = 0;
    keep_fds[keep_count++] = reply_fd;
    if (retained_received_fd >= 16) keep_fds[keep_count++] = retained_received_fd;
    if ((ioctl_attachments.consumed_fd_flags & DRMD_IOCTL_FD_INPUT_WAIT) != 0) {
        keep_fds[keep_count++] = ioctl_attachments.input_wait_fd;
    }
    if ((ioctl_attachments.consumed_fd_flags & DRMD_IOCTL_FD_OUTPUT_NOTIFY) != 0) {
        keep_fds[keep_count++] = ioctl_attachments.output_notify_fd;
    }
    if (status == DRMD_IOCTL_DEFERRED) {
        if (service->deferred.active) {
            status = -16;
        } else {
            service->deferred.active = 1;
            service->deferred.page_fd = page_fd;
            service->deferred.reply_fd = reply_fd;
            service->deferred.page = page;
            service->deferred.header = header;
            keep_fds[keep_count++] = page_fd;
            close_received(request, fds, keep_fds, keep_count);
            return 0;
        }
    }
    if (status == 0) drmd_drm_island_notify_readable(service->drm);
    close_received(request, fds, keep_fds, keep_count);
    const int reply_status = send_reply(
        reply_fd, page, &header, status, result, transfer_fd, transfer_rights, transfer_flags);
    if (reply_status != 0 && prime_export_ref) {
        (void)drmd_drm_island_prime_release(service->drm, result);
    }
    (void)pacha_munmap(page, DRMD_PAGE_BYTES);
    return reply_status;
}

int drmd_service_progress(drmd_service_t *service)
{
    if (service == NULL || !service->deferred.active) return 0;
    int status = 0;
    if (!drmd_kms_take_deferred_ioctl_result(&status)) return 0;
    const int reply_status = send_reply(
        service->deferred.reply_fd,
        service->deferred.page,
        &service->deferred.header,
        status,
        0,
        0,
        0,
        0);
    (void)pacha_munmap(service->deferred.page, DRMD_PAGE_BYTES);
    (void)pacha_fd_close(service->deferred.page_fd);
    memset(&service->deferred, 0, sizeof(service->deferred));
    service->deferred.page_fd = -1;
    service->deferred.reply_fd = -1;
    return reply_status;
}
