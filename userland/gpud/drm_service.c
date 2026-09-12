/* SPDX-License-Identifier: MIT */
#include "drm_service.h"
#include "drm_control.h"
#include "drm_reply.h"
#include "../kobox2_adapter/gpu_query_message.h"

#include <errno.h>
#include <pacha/ipc.h>
#include <pacha/syscall.h>
#include <stdio.h>
#include <string.h>

static int valid_fd(uint64_t fd, uint64_t kind, uint64_t rights, uint64_t size) {
    struct pacha_fd_info info = {0};
    return fd >= 16 && fd < PACHA_FD_TABLE_LIMIT &&
        !pacha_syscall2(PACHA_FD_SYSCALL_GET_INFO, fd, (uintptr_t)&info) &&
        info.kind == kind && (info.rights & rights) == rights && (!size || info.size == size);
}

int gpud_drm_service_bind(struct gpud_drm_service *service, int endpoint_fd) {
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_RECV | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL;
    if (!service || service->endpoint_fd || !service->files.generation ||
        endpoint_fd < 16 || endpoint_fd >= PACHA_FD_TABLE_LIMIT)
        return -EINVAL;
    if (!valid_fd(endpoint_fd, PACHA_FD_KIND_ENDPOINT, rights, 0))
        return -EACCES;
    service->endpoint_fd = endpoint_fd;
    return 0;
}

static struct gpud_drm_watch *empty_watch(struct gpud_drm_service *service) {
    for (size_t i = 0; i < GPUD_DRM_REFERENCES_MAX; ++i)
        if (!service->watches[i].handle)
            return &service->watches[i];
    return NULL;
}

static const struct gpud_drm_file *find_file(
    const struct gpud_drm_service *service, uint64_t handle) {
    for (size_t i = 0; i < service->files.limit; ++i)
        if (service->files.files[i].handle == handle)
            return &service->files.files[i];
    return NULL;
}

static int retire_owner_watch(struct gpud_drm_service *service, uint64_t handle) {
    for (size_t i = 0; i < GPUD_DRM_REFERENCES_MAX; ++i) {
        struct gpud_drm_watch *watch = &service->watches[i];
        if (watch->handle != handle || watch->transferred)
            continue;
        if (pacha_fd_close(watch->fd))
            return gpud_drm_files_fault(&service->files, service->files.generation, -EIO);
        memset(watch, 0, sizeof(*watch));
        return 0;
    }
    return 0;
}

static int control(struct gpud_drm_service *service, struct gpud_drm_control *pending,
    size_t size, uint64_t *handle) {
    unsigned char reply[GPUD_GPU_CHANNEL_PAGE];
    size_t reply_size;
    int error = gpud_gpu_rpc_call(&service->gpu, GPUD_GPU_QUEUE_CONTROL,
        service->gpu.mapping + GPUD_GPU_CONTROL_REQUEST_OFFSET, size,
        reply, sizeof(reply), &reply_size);
    if (error)
        return gpud_drm_files_fault(&service->files, service->files.generation, error);
    return gpud_drm_control_complete(&service->files, pending, reply, reply_size, handle);
}

static int close_draining(struct gpud_drm_service *service) {
    for (;;) {
        struct gpud_drm_control pending = {0};
        size_t size;
        if (service->correlation == UINT64_MAX)
            return -EOVERFLOW;
        int result = gpud_drm_close_prepare(&service->files, service->files.generation,
            ++service->correlation, &pending,
            service->gpu.mapping + GPUD_GPU_CONTROL_REQUEST_OFFSET, 2048, &size);
        if (result != 1)
            return result;
        uint64_t handle = 0;
        result = control(service, &pending, size, &handle);
        if (result)
            return result;
        for (size_t i = 0; i < GPUD_DRM_REFERENCES_MAX; ++i) {
            if (service->watches[i].handle != handle)
                continue;
            if (pacha_fd_close(service->watches[i].fd))
                return -EIO;
            memset(&service->watches[i], 0, sizeof(service->watches[i]));
        }
    }
}

static int ioctl_request(
    struct gpud_drm_service *service, drmd_ioctl_request_t *request) {
    struct gpud_drm_binding binding;
    uint64_t generation = service->files.generation;
    int error = gpud_drm_file_acquire(
        &service->files, generation, request->handle, &binding);
    if (error)
        return error;
    struct gpud_drm_translation translation;
    error = gpud_drm_ioctl_encode(&translation, &binding, request, PH_GPU_QUERY_OUTPUT_REGION);
    if (!error) {
        unsigned char bytes[KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + GPUD_DRM_COMMAND_BYTES];
        unsigned char reply[GPUD_GPU_CHANNEL_PAGE], output[GPUD_DRM_VERSION_BYTES];
        size_t size = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + translation.command_size, reply_size;
        kb2_protocol_message_envelope_t envelope = {.protocol_id = KB2_GPU_PROTOCOL_ID,
            .opcode = KB2_GPU_OPCODE_COMMAND, .generation = generation,
            .correlation_id = service->correlation, .payload_length = translation.command_size};
        error = kb2_protocol_message_envelope_encode(bytes, size, &envelope) ? -EPROTO : 0;
        if (!error) {
            memcpy(bytes + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
                translation.command, translation.command_size);
            error = gpud_gpu_rpc_call(&service->gpu, GPUD_GPU_QUEUE_EXECUTION,
                bytes, size, reply, sizeof(reply), &reply_size);
            if (error)
                gpud_drm_files_fault(&service->files, generation, error);
            else {
                memcpy(output, service->gpu.mapping + GPUD_GPU_OUTPUT_OFFSET, sizeof(output));
                error = gpud_drm_ioctl_reply(request, &translation, service->correlation,
                    reply, reply_size, output, sizeof(output));
                if (error == -EPROTO)
                    gpud_drm_files_fault(&service->files, generation, error);
            }
        }
    }
    int release = gpud_drm_file_release(&service->files, generation, request->handle);
    return release ? release : error;
}

static int dispatch(struct gpud_drm_service *service,
    const pacha_service_envelope_t *header, void *payload, uint64_t *result) {
    uint64_t generation = service->files.generation;
    size_t count = service->received.fd_count;
    if (service->files.terminal_error)
        return service->files.terminal_error;
    if (service->correlation == UINT64_MAX)
        return -EOVERFLOW;
    ++service->correlation;
    if (header->op == DRMD_OP_HELLO)
        return header->payload_size || count != 2 ? -EINVAL : (*result = 1, 0);
    if (header->op == DRMD_OP_OPEN_NODE) {
        if (header->payload_size != sizeof(drmd_open_request_t) || count != 3 ||
            !valid_fd(service->received.fds[1].fd, PACHA_FD_KIND_CHANNEL,
                PH_IPC_CHANNEL_RIGHTS | PACHA_FD_RIGHT_CLOSE, 0))
            return -EINVAL;
        struct gpud_drm_watch *watch = empty_watch(service);
        if (!watch)
            return -EMFILE;
        struct gpud_drm_control pending = {0};
        size_t size;
        int error = gpud_drm_open_prepare(&service->files, generation,
            service->backend_client, service->correlation, payload, &pending,
            service->gpu.mapping + GPUD_GPU_CONTROL_REQUEST_OFFSET, 2048, &size);
        if (!error)
            error = control(service, &pending, size, result);
        if (!error) {
            *watch = (struct gpud_drm_watch){.handle = *result,
                .fd = (int)service->received.fds[1].fd};
            service->received.fds[1].fd = PH_IPC_NO_FD;
        }
        return error;
    }
    if (header->op == DRMD_OP_HANDLE_IOCTL) {
        if (count != 2 || header->payload_size != sizeof(drmd_ioctl_request_t))
            return -EINVAL;
        return ioctl_request(service, payload);
    }
    if (header->op == DRMD_OP_HANDLE_CLOSE || header->op == DRMD_OP_HANDLE_DUP) {
        const drmd_handle_request_t *request = payload;
        if (header->payload_size != sizeof(*request) || request->arg0 || request->arg1 || request->arg2)
            return -EINVAL;
        if (header->op == DRMD_OP_HANDLE_DUP) {
            if (count != 2 && count != 3)
                return -EOPNOTSUPP;
            struct gpud_drm_watch *watch = NULL;
            if (count == 3) {
                uint64_t fd = service->received.fds[1].fd;
                if (fd == service->received.fds[0].fd ||
                    fd == service->received.fds[2].fd ||
                    !valid_fd(fd, PACHA_FD_KIND_CHANNEL,
                        PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL, 0))
                    return -EINVAL;
                watch = empty_watch(service);
                if (!watch)
                    return -EMFILE;
            }
            int error = gpud_drm_file_dup(
                &service->files, generation, request->handle);
            if (!error) {
                *result = request->handle;
                if (watch) {
                    *watch = (struct gpud_drm_watch){.handle = request->handle,
                        .fd = (int)service->received.fds[1].fd, .transferred = 1};
                    service->received.fds[1].fd = PH_IPC_NO_FD;
                }
            }
            return error;
        }
        if (count != 2)
            return -EOPNOTSUPP;
        int error = gpud_drm_file_close(
            &service->files, generation, request->handle);
        if (!error)
            error = retire_owner_watch(service, request->handle);
        return error ? error : close_draining(service);
    }
    return -EOPNOTSUPP;
}

size_t gpud_drm_service_collect_wait_sources(
    const struct gpud_drm_service *service, int *fds, size_t capacity) {
    if (!service || !fds)
        return 0;
    size_t count = 0;
    for (size_t i = 0; i < GPUD_DRM_REFERENCES_MAX && count < capacity; ++i)
        if (service->watches[i].handle && service->watches[i].fd >= 16)
            fds[count++] = service->watches[i].fd;
    return count;
}

int gpud_drm_service_reap_hangups(struct gpud_drm_service *service) {
    if (!service || service->error || service->files.terminal_error)
        return service ? (service->error ? service->error : service->files.terminal_error) : -EINVAL;
    for (size_t i = 0; i < GPUD_DRM_REFERENCES_MAX; ++i) {
        struct gpud_drm_watch *watch = &service->watches[i];
        if (!watch->handle || watch->fd < 16)
            continue;
        struct pacha_pollfd poll = {.fd = watch->fd, .events = PACHA_FD_EVENT_HANGUP};
        if (pacha_fd_poll(&poll, 1) <= 0 || !(poll.revents & PACHA_FD_EVENT_HANGUP))
            continue;
        uint64_t handle = watch->handle;
        if (pacha_fd_close(watch->fd))
            return service->error =
                gpud_drm_files_fault(&service->files, service->files.generation, -EIO);
        unsigned int transferred = watch->transferred;
        memset(watch, 0, sizeof(*watch));
        int error = gpud_drm_file_close(
            &service->files, service->files.generation, handle);
        const struct gpud_drm_file *file = error ? NULL : find_file(service, handle);
        int last = file && file->state == GPUD_DRM_FILE_DRAINING;
        if (!error)
            error = close_draining(service);
        if (error)
            return service->error = error;
        if (!transferred && last && !find_file(service, handle))
            printf("[gpud] drm client-hangup handle=%llu generation=%llu backend-close=1\n",
                (unsigned long long)handle,
                (unsigned long long)service->files.generation);
    }
    return 0;
}

int gpud_drm_service_receive(struct gpud_drm_service *service) {
    if (!service || service->endpoint_fd < 16 || !service->backend_client)
        return -EINVAL;
    if (service->error)
        return service->error;
    struct pacha_ipc_msg message = {.fds = service->received.fds,
        .fd_capacity = PACHA_IPC_MAX_TRANSFER_FDS};
    long native = pacha_syscall2(
        PACHA_IPC_SYSCALL_RECV, service->endpoint_fd, (uintptr_t)&message);
    if (native)
        return native == PACHA_SYSCALL_ERR_EMPTY || native == PACHA_SYSCALL_ERR_NOT_READY ?
            -EAGAIN : (service->error = -EIO);
    service->received.fd_count = message.fd_count;
    int error = -EPROTO;
    if (message.fd_count < 2)
        goto cleanup;
    uint64_t page_fd = message.fds[0].fd, reply_fd = message.fds[message.fd_count - 1].fd;
    if (!valid_fd(page_fd, PACHA_FD_KIND_VMO,
            PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE, DRMD_PAGE_BYTES) ||
        !valid_fd(reply_fd, PACHA_FD_KIND_REPLY, PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_SEND, 0))
        goto cleanup;
    long address = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, page_fd, 0, DRMD_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (address < 4096)
        goto cleanup;
    service->page = (void *)(uintptr_t)address;
    pacha_service_envelope_t header, response;
    union { drmd_ioctl_request_t ioctl; drmd_open_request_t open; drmd_handle_request_t handle; } payload;
    memcpy(&header, service->page, sizeof(header));
    int status = -EINVAL;
    uint64_t result = 0;
    uint32_t response_size = 0;
    if (message.word0 == PACHA_SERVICE_REQUEST_MAGIC && !message.word1 && !message.word2 &&
        message.word3 == header.request_id && pacha_service_request_is_valid(&header, DRMD_SERVICE_ID) &&
        header.flags == (header.payload_size ? PACHA_SERVICE_FLAG_PAGE_PAYLOAD : 0) &&
        !header.fd_count && !header.reserved0 && !header.reserved1 && header.payload_size <= sizeof(payload)) {
        memcpy(&payload, (unsigned char *)service->page + sizeof(header), header.payload_size);
        status = dispatch(service, &header, &payload, &result);
        if (!status && header.op == DRMD_OP_HANDLE_IOCTL) {
            response_size = sizeof(payload.ioctl);
            memcpy((unsigned char *)service->page + sizeof(header), &payload, response_size);
        }
    }
    pacha_service_reply_init(&response, &header, status, PACHA_SERVICE_ERROR_DRMD_DRM,
        status ? 0 : result, response_size);
    memcpy(service->page, &response, sizeof(response));
    struct pacha_ipc_msg reply = {.word0 = PACHA_SERVICE_REPLY_MAGIC,
        .word1 = (uint64_t)status, .word2 = status ? 0 : result, .word3 = header.request_id};
    error = pacha_syscall2(PACHA_IPC_SYSCALL_REPLY, reply_fd, (uintptr_t)&reply) ? -EIO : 0;
cleanup:
    if (service->page) {
        if (pacha_syscall2(PACHA_VM_SYSCALL_MUNMAP, (uintptr_t)service->page, DRMD_PAGE_BYTES))
            error = -EIO;
        else
            service->page = NULL;
    }
    int release = ph_ipc_packet_release(&service->received);
    if (release)
        error = release;
    if (service->files.terminal_error)
        error = service->files.terminal_error;
    service->error = error;
    return error;
}
