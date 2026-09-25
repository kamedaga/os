/* SPDX-License-Identifier: MIT */
#include "kobox2_frame_client.h"

#include <pacha/ipc.h>
#include <errno.h>
#include <string.h>

static int wait_reply(struct ph_ipc *ipc, int process_fd,
    uint64_t operation, uint64_t correlation)
{
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
                events[0].revents != PACHA_FD_EVENT_READABLE)
                return -EPIPE;
            continue;
        }
        if (result) return result;
        result = reply.operation == operation &&
            reply.generation == ipc->generation &&
            reply.correlation == correlation && !reply.fd_count ?
            (int)(int64_t)reply.value : -EPROTO;
        int closed = ph_ipc_packet_release(&reply);
        return closed ? closed : result;
    }
}

static int request(struct netd_kobox2_frame_client *client,
    struct ph_ipc *ipc, int process_fd, uint64_t operation, int attach_fd)
{
    if (!client || !client->shared || !ipc ||
        client->generation != ipc->generation || process_fd < 16)
        return -EINVAL;
    uint64_t correlation = ++client->next_correlation;
    if (!correlation) return -EOVERFLOW;
    client->shared->operation = operation;
    client->shared->status = 0;
    struct ph_ipc_packet packet = {.operation = operation,
        .generation = client->generation, .correlation = correlation,
        .value = attach_fd >= 16 ? PH_NET_FRAME_SHARED_BYTES : 0};
    if (attach_fd >= 16) {
        packet.fd_count = 1;
        packet.fds[0] = (struct pacha_ipc_fd){.fd = (uint64_t)attach_fd,
            .rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
                PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE,
            .flags = PACHA_FD_FLAG_CLOEXEC};
    }
    int result = ph_ipc_send(ipc, &packet);
    if (result) return result;
    result = wait_reply(ipc, process_fd, operation, correlation);
    if (result) return result;
    if (client->shared->magic != PH_NET_FRAME_SHARED_MAGIC ||
        client->shared->generation != client->generation ||
        client->shared->operation != operation)
        return -EPROTO;
    return client->shared->status;
}

int netd_kobox2_frame_open(struct netd_kobox2_frame_client *client,
    struct ph_ipc *ipc, int process_fd, uint64_t generation)
{
    if (!client || client->shared || !ipc || !generation ||
        generation != ipc->generation) return -EINVAL;
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    int fd = pacha_vmo_create(PH_NET_FRAME_SHARED_BYTES, rights, 0);
    if (fd < 16) return fd < 0 ? fd : -ENOMEM;
    void *mapped = pacha_mmap(fd, PH_NET_FRAME_SHARED_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (!mapped) {
        (void)pacha_fd_close(fd);
        return -ENOMEM;
    }
    memset(mapped, 0, PH_NET_FRAME_SHARED_BYTES);
    client->vmo_fd = fd;
    client->shared = mapped;
    client->generation = generation;
    client->shared->magic = PH_NET_FRAME_SHARED_MAGIC;
    client->shared->generation = generation;
    int result = request(client, ipc, process_fd, PH_NET_FRAME_ATTACH, fd);
    if (result) (void)netd_kobox2_frame_close(client);
    return result;
}

int netd_kobox2_frame_info(struct netd_kobox2_frame_client *client,
    struct ph_ipc *ipc, int process_fd, struct kobox_linux_net_info *info)
{
    if (!info) return -EINVAL;
    int result = request(client, ipc, process_fd, PH_NET_FRAME_INFO, -1);
    if (!result) *info = client->shared->info;
    return result;
}

int netd_kobox2_frame_read(struct netd_kobox2_frame_client *client,
    struct ph_ipc *ipc, int process_fd, const struct kobox_linux_net_frame **frames,
    size_t *count)
{
    if (!frames || !count) return -EINVAL;
    int result = request(client, ipc, process_fd, PH_NET_FRAME_READ, -1);
    if (result) return result;
    if (client->shared->count > PH_NET_FRAME_BATCH) return -EPROTO;
    *frames = client->shared->frames;
    *count = client->shared->count;
    return 0;
}

int netd_kobox2_frame_write(struct netd_kobox2_frame_client *client,
    struct ph_ipc *ipc, int process_fd, const void *frame, size_t length)
{
    if (!client || !client->shared || !frame ||
        length < 14 || length > KOBOX_NET_FRAME_MAX) return -EMSGSIZE;
    client->shared->count = 1;
    client->shared->frames[0].length = length;
    memcpy(client->shared->frames[0].bytes, frame, length);
    int result = request(client, ipc, process_fd, PH_NET_FRAME_WRITE, -1);
    if (!result && client->shared->count != 1) return -EPROTO;
    return result;
}

int netd_kobox2_frame_close(struct netd_kobox2_frame_client *client)
{
    if (!client) return -EINVAL;
    int result = 0;
    if (client->shared)
        result = pacha_munmap(client->shared, PH_NET_FRAME_SHARED_BYTES);
    if (client->vmo_fd >= 16) {
        int closed = pacha_fd_close(client->vmo_fd);
        if (!result) result = closed;
    }
    *client = (struct netd_kobox2_frame_client){0};
    return result;
}
