/* SPDX-License-Identifier: MIT */
#include "block_client.h"

#include <errno.h>
#include <string.h>

static int status_errno(uint32_t status) {
    switch (status) {
    case KB2_BLOCK_DEVICE_STATUS_OK: return 0;
    case KB2_BLOCK_DEVICE_STATUS_INVALID: return -EINVAL;
    case KB2_BLOCK_DEVICE_STATUS_STALE: return -ESTALE;
    case KB2_BLOCK_DEVICE_STATUS_NO_DEVICE: return -ENODEV;
    case KB2_BLOCK_DEVICE_STATUS_READ_ONLY: return -EACCES;
    case KB2_BLOCK_DEVICE_STATUS_NO_SPACE: return -ENOSPC;
    case KB2_BLOCK_DEVICE_STATUS_UNSUPPORTED: return -EOPNOTSUPP;
    default: return -EIO;
    }
}

static int wait_reply(struct ph_ipc *ipc, pacha_fd_t process_fd,
    uint64_t operation, uint64_t sequence, uint64_t *status) {
    for (;;) {
        struct ph_ipc_packet reply = {0};
        int result = ph_ipc_receive(ipc, ipc->generation, &reply);
        if (result == -EAGAIN) {
            struct pacha_pollfd events[2] = {
                {.fd = ipc->fd, .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP},
                {.fd = process_fd, .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP},
            };
            long waited = pacha_fd_wait_many(events, 2, PACHA_FD_WAIT_FOREVER);
            if (waited <= 0 || events[1].revents ||
                events[0].revents != PACHA_FD_EVENT_READABLE) return -EPIPE;
            continue;
        }
        if (result) return result;
        result = reply.operation == operation &&
            reply.generation == ipc->generation &&
            reply.correlation == sequence && !reply.fd_count ? 0 : -EPROTO;
        if (!result) *status = reply.value;
        int closed = ph_ipc_packet_release(&reply);
        return closed ? closed : result;
    }
}

static int issue(struct ph_block_client *client, struct ph_ipc *ipc,
    pacha_fd_t process_fd, kb2_block_header_t *header, pacha_fd_t attach_fd) {
    if (!client || !client->shared || !ipc ||
        client->generation != ipc->generation || process_fd < 16)
        return -EINVAL;
    uint64_t sequence = ++client->sequence;
    if (!sequence) return -EOVERFLOW;
    uint64_t operation = PH_BLOCK_ATTACH + (header ? header->opcode : 0);
    if (header) {
        header->generation = client->generation;
        header->sequence = sequence;
        header->status = 0;
        if (kb2_block_header_encode(client->shared, PH_BLOCK_SHARED_BYTES, header))
            return -EINVAL;
    }
    struct ph_ipc_packet packet = {.operation = operation,
        .generation = client->generation, .correlation = sequence,
        .value = attach_fd >= 16 ? PH_BLOCK_SHARED_BYTES : 0};
    if (attach_fd >= 16) {
        packet.fd_count = 1;
        packet.fds[0] = (struct pacha_ipc_fd){.fd = (uint64_t)attach_fd,
            .rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
                PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE,
            .flags = PACHA_FD_FLAG_CLOEXEC};
    }
    int result = ph_ipc_send(ipc, &packet);
    if (result) return result;
    uint64_t reply_status = 0;
    result = wait_reply(ipc, process_fd, operation, sequence, &reply_status);
    if (result) return result;
    if (!header) return reply_status ? -EIO : 0;
    kb2_block_header_t response = {0};
    if (kb2_block_header_decode(client->shared, PH_BLOCK_SHARED_BYTES, &response) ||
        response.opcode != header->opcode ||
        response.generation != client->generation ||
        response.sequence != sequence || response.status != reply_status ||
        response.diskseq != header->diskseq ||
        response.major != header->major || response.minor != header->minor)
        return -EPROTO;
    if (!response.status && header->opcode == KB2_BLOCK_DEVICE_OP_SNAPSHOT) {
        if (response.count > header->count ||
            (response.offset &&
             (header->offset > UINT64_MAX - response.count ||
              response.offset != header->offset + response.count)))
            return -EPROTO;
    } else if (!response.status &&
               (response.count || response.offset != header->offset ||
                response.length != header->length)) {
        return -EPROTO;
    }
    *header = response;
    return status_errno(response.status);
}

int ph_block_client_open(struct ph_block_client *client, struct ph_ipc *ipc,
    pacha_fd_t process_fd, uint64_t generation) {
    if (!client || client->shared || !ipc || !generation ||
        generation != ipc->generation) return -EINVAL;
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    pacha_fd_t fd = pacha_vmo_create(PH_BLOCK_SHARED_BYTES, rights, 0);
    if (fd < 16) return fd < 0 ? fd : -ENOMEM;
    void *mapped = pacha_mmap(fd, PH_BLOCK_SHARED_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (!mapped) {
        (void)pacha_fd_close(fd);
        return -ENOMEM;
    }
    memset(mapped, 0, PH_BLOCK_SHARED_BYTES);
    *client = (struct ph_block_client){.shared = mapped, .vmo_fd = fd,
        .generation = generation};
    int result = issue(client, ipc, process_fd, NULL, fd);
    if (result) (void)ph_block_client_close(client);
    return result;
}

int ph_block_client_snapshot(struct ph_block_client *client, struct ph_ipc *ipc,
    pacha_fd_t process_fd, uint64_t start, kb2_block_disk_t *disks,
    size_t capacity, size_t *count, uint64_t *next) {
    if (!disks || !count || !next || !capacity ||
        capacity > KB2_BLOCK_DEVICE_MAX_DISKS) return -EINVAL;
    kb2_block_header_t header = {.opcode = KB2_BLOCK_DEVICE_OP_SNAPSHOT,
        .offset = start, .count = (uint32_t)capacity};
    int result = issue(client, ipc, process_fd, &header, -1);
    if (result) return result;
    if (header.count > capacity) return -EPROTO;
    for (size_t i = 0; i < header.count; ++i) {
        result = kb2_block_disk_decode(client->shared +
            KB2_BLOCK_DEVICE_HEADER_SIZE + i * KB2_BLOCK_DEVICE_DISK_SIZE,
            PH_BLOCK_SHARED_BYTES - KB2_BLOCK_DEVICE_HEADER_SIZE -
                i * KB2_BLOCK_DEVICE_DISK_SIZE, &disks[i]);
        if (result) return -EPROTO;
    }
    *count = header.count;
    *next = header.offset;
    return 0;
}

static int transfer(struct ph_block_client *client, struct ph_ipc *ipc,
    pacha_fd_t process_fd, const kb2_block_disk_t *disk, uint64_t offset,
    void *buffer, size_t length, uint32_t opcode) {
    if (!client || !client->shared || !disk || !buffer || !length ||
        length > KB2_BLOCK_DEVICE_MAX_TRANSFER ||
        !disk->logical_block_size || offset % disk->logical_block_size ||
        length % disk->logical_block_size) return -EINVAL;
    kb2_block_header_t header = {.opcode = opcode, .diskseq = disk->diskseq,
        .major = disk->major, .minor = disk->minor,
        .offset = offset, .length = (uint32_t)length};
    if (opcode == KB2_BLOCK_DEVICE_OP_WRITE)
        memcpy(client->shared + KB2_BLOCK_DEVICE_HEADER_SIZE, buffer, length);
    int result = issue(client, ipc, process_fd, &header, -1);
    if (!result && opcode == KB2_BLOCK_DEVICE_OP_READ)
        memcpy(buffer, client->shared + KB2_BLOCK_DEVICE_HEADER_SIZE, length);
    return result;
}

int ph_block_client_read(struct ph_block_client *client, struct ph_ipc *ipc,
    pacha_fd_t process_fd, const kb2_block_disk_t *disk, uint64_t offset,
    void *buffer, size_t length) {
    return transfer(client, ipc, process_fd, disk, offset, buffer, length,
        KB2_BLOCK_DEVICE_OP_READ);
}

int ph_block_client_write(struct ph_block_client *client, struct ph_ipc *ipc,
    pacha_fd_t process_fd, const kb2_block_disk_t *disk, uint64_t offset,
    const void *buffer, size_t length) {
    return transfer(client, ipc, process_fd, disk, offset, (void *)buffer, length,
        KB2_BLOCK_DEVICE_OP_WRITE);
}

int ph_block_client_flush(struct ph_block_client *client, struct ph_ipc *ipc,
    pacha_fd_t process_fd, const kb2_block_disk_t *disk) {
    if (!disk) return -EINVAL;
    kb2_block_header_t header = {.opcode = KB2_BLOCK_DEVICE_OP_FLUSH,
        .diskseq = disk->diskseq, .major = disk->major, .minor = disk->minor};
    return issue(client, ipc, process_fd, &header, -1);
}

int ph_block_client_close(struct ph_block_client *client) {
    if (!client) return -EINVAL;
    int result = 0;
    if (client->shared) result = pacha_munmap(client->shared, PH_BLOCK_SHARED_BYTES);
    if (client->vmo_fd >= 16) {
        int closed = pacha_fd_close(client->vmo_fd);
        if (!result) result = closed;
    }
    *client = (struct ph_block_client){0};
    return result;
}
