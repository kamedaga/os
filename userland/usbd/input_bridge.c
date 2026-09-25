/* SPDX-License-Identifier: MIT */
#include "input_bridge.h"

#include <pacha/ipc.h>
#include <errno.h>
#include <string.h>

static int wait_reply(struct ph_ipc *ipc, int process_fd, uint64_t operation,
    uint64_t correlation) {
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

static int request(struct usbd_input_bridge *bridge, struct ph_ipc *ipc,
    int process_fd, uint64_t operation, int attach_fd) {
    if (!bridge || !bridge->shared || !ipc ||
        bridge->generation != ipc->generation || process_fd < 16)
        return -EINVAL;
    uint64_t correlation = ++bridge->next_correlation;
    if (!correlation) return -EOVERFLOW;
    struct ph_ipc_packet packet = {.operation = operation,
        .generation = bridge->generation, .correlation = correlation,
        .value = attach_fd >= 16 ? PH_USB_INPUT_SHARED_BYTES : 0};
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
    if (bridge->shared->magic != PH_USB_INPUT_SHARED_MAGIC ||
        bridge->shared->generation != bridge->generation ||
        bridge->shared->operation != operation)
        return -EPROTO;
    return bridge->shared->status;
}

int usbd_input_bridge_open(struct usbd_input_bridge *bridge,
    struct ph_ipc *ipc, int process_fd, uint64_t generation) {
    if (!bridge || bridge->shared || !ipc || !generation ||
        generation != ipc->generation) return -EINVAL;
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    int fd = pacha_vmo_create(PH_USB_INPUT_SHARED_BYTES, rights, 0);
    if (fd < 16) return fd < 0 ? fd : -ENOMEM;
    void *mapped = pacha_mmap(fd, PH_USB_INPUT_SHARED_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (!mapped) {
        (void)pacha_fd_close(fd);
        return -ENOMEM;
    }
    memset(mapped, 0, PH_USB_INPUT_SHARED_BYTES);
    bridge->vmo_fd = fd;
    bridge->shared = mapped;
    bridge->generation = generation;
    bridge->shared->magic = PH_USB_INPUT_SHARED_MAGIC;
    bridge->shared->generation = generation;
    int result = request(bridge, ipc, process_fd, PH_USB_INPUT_ATTACH, fd);
    if (result) (void)usbd_input_bridge_close(bridge);
    return result;
}

int usbd_input_bridge_snapshot(struct usbd_input_bridge *bridge,
    struct ph_ipc *ipc, int process_fd,
    const struct kobox_linux_input_device_info **devices,
    size_t *count, uint64_t *next_sequence) {
    if (!devices || !count || !next_sequence) return -EINVAL;
    int result = request(bridge, ipc, process_fd, PH_USB_INPUT_SNAPSHOT, -1);
    if (result) return result;
    size_t capacity = (PH_USB_INPUT_SHARED_BYTES -
        offsetof(struct ph_usb_input_shared, payload)) /
        sizeof(struct kobox_linux_input_device_info);
    if (bridge->shared->count > capacity) return -EPROTO;
    *devices = (const void *)bridge->shared->payload;
    *count = bridge->shared->count;
    *next_sequence = bridge->shared->sequence;
    return 0;
}

int usbd_input_bridge_read(struct usbd_input_bridge *bridge,
    struct ph_ipc *ipc, int process_fd,
    const struct kobox_linux_input_record **records,
    size_t *count, uint64_t *overwritten) {
    if (!records || !count || !overwritten) return -EINVAL;
    int result = request(bridge, ipc, process_fd, PH_USB_INPUT_READ, -1);
    if (result) return result;
    if (bridge->shared->count > PH_USB_INPUT_READ_MAX) return -EPROTO;
    *records = (const void *)bridge->shared->payload;
    *count = bridge->shared->count;
    *overwritten = bridge->shared->overwritten;
    return 0;
}

int usbd_input_bridge_close(struct usbd_input_bridge *bridge) {
    if (!bridge) return -EINVAL;
    int result = 0;
    if (bridge->shared)
        result = pacha_munmap(bridge->shared, PH_USB_INPUT_SHARED_BYTES);
    if (bridge->vmo_fd >= 16) {
        int closed = pacha_fd_close(bridge->vmo_fd);
        if (!result) result = closed;
    }
    *bridge = (struct usbd_input_bridge){0};
    return result;
}
