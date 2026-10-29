/* SPDX-License-Identifier: MIT */
#include "kobox2_frame_client.h"

#include <pacha/ipc.h>
#include <errno.h>
#include <string.h>
#include <stdatomic.h>

static int wait_reply(struct ph_ipc *ipc, pacha_fd_t process_fd,
    uint64_t operation, uint64_t correlation)
{
    for (;;) {
        struct ph_ipc_packet reply = {0};
        /* The reply endpoint is the sole progress source. Bound the native
         * IPC wait so process death is still detected if a peer retains a
         * channel reference after the sandbox exits. */
        int result = ph_ipc_receive_wait(ipc, ipc->generation, &reply, 100);
        if (result == -EAGAIN) {
            struct pacha_pollfd process = {.fd = process_fd,
                .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP};
            long polled = pacha_fd_poll(&process, 1);
            if (polled != 0 || process.revents)
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
    struct ph_ipc *ipc, pacha_fd_t process_fd, uint64_t operation, pacha_fd_t attach_fd)
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
    atomic_thread_fence(memory_order_acquire);
    if (client->shared->magic != PH_NET_FRAME_SHARED_MAGIC ||
        client->shared->generation != client->generation ||
        client->shared->operation != operation)
        return -EPROTO;
    return client->shared->status;
}

int netd_kobox2_frame_open(struct netd_kobox2_frame_client *client,
    struct ph_ipc *ipc, pacha_fd_t process_fd, uint64_t generation)
{
    if (!client || client->shared || !ipc || !generation ||
        generation != ipc->generation) return -EINVAL;
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    pacha_fd_t fd = pacha_vmo_create(PH_NET_FRAME_SHARED_BYTES, rights, 0);
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
    struct ph_ipc *ipc, pacha_fd_t process_fd, struct kobox_linux_net_info *info)
{
    if (!info) return -EINVAL;
    int result = request(client, ipc, process_fd, PH_NET_FRAME_INFO, -1);
    if (!result) *info = client->shared->info;
    return result;
}

int netd_kobox2_frame_read(struct netd_kobox2_frame_client *client,
    struct ph_ipc *ipc, pacha_fd_t process_fd, const struct kobox_linux_net_frame **frames,
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
    struct ph_ipc *ipc, pacha_fd_t process_fd, const void *frame, size_t length)
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

static void wipe_wifi(struct netd_kobox2_frame_client *client)
{
    for (size_t i = 0; i < PH_NET_WIFI_BYTES; ++i)
        ((volatile unsigned char *)client->shared->wifi_bytes)[i] = 0;
    client->shared->wifi_length = 0;
}

int netd_kobox2_wifi_request(struct netd_kobox2_frame_client *client,
    struct ph_ipc *ipc, pacha_fd_t process_fd, const void *command, size_t length,
    void *reply, size_t capacity, size_t *received)
{
    if (!client || !client->shared || !command || !length || length > PH_NET_WIFI_BYTES ||
        !reply || !capacity || capacity > PH_NET_WIFI_BYTES || !received) return -EINVAL;
    *received = 0;
    memcpy(client->shared->wifi_bytes, command, length);
    client->shared->wifi_length = (uint32_t)length;
    int result = request(client, ipc, process_fd, PH_NET_WIFI_REQUEST, -1);
    if (!result) {
        size_t count = client->shared->wifi_length;
        if (count > PH_NET_WIFI_BYTES) result = -EPROTO;
        else if (count > capacity) result = -EMSGSIZE;
        else { memcpy(reply, client->shared->wifi_bytes, count); *received = count; }
    }
    wipe_wifi(client);
    return result;
}

int netd_kobox2_wifi_configure(struct netd_kobox2_frame_client *client,
    struct ph_ipc *ipc, pacha_fd_t process_fd, const void *configuration, size_t length)
{
    if (!client || !client->shared || !configuration || !length || length > PH_NET_WIFI_BYTES)
        return -EINVAL;
    memcpy(client->shared->wifi_bytes, configuration, length);
    client->shared->wifi_length = (uint32_t)length;
    int result = request(client, ipc, process_fd, PH_NET_WIFI_CONFIG, -1);
    wipe_wifi(client);
    return result;
}

int netd_kobox2_wifi_events(struct netd_kobox2_frame_client *client,
    struct ph_ipc *ipc, pacha_fd_t process_fd, uint32_t *events)
{
    if (!client || !client->shared || !events) return -EINVAL;
    client->shared->wifi_events = 0;
    int result = request(client, ipc, process_fd, PH_NET_WIFI_EVENTS, -1);
    if (!result) *events = client->shared->wifi_events;
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
