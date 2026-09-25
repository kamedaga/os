/* SPDX-License-Identifier: MIT */
#include "usb_input_service.h"
#include "host.h"

#include <pacha/syscall.h>
#include <errno.h>
#include <string.h>

static int prepare(void *context, const struct ph_ipc_packet *packet) {
    struct ph_usb_input_service *input = context;
    if (!packet->correlation || packet->generation != input->generation ||
        input->operation) return -EPROTO;
    switch (packet->operation) {
    case PH_USB_INPUT_ATTACH: {
        struct pacha_fd_info info = {0};
        if (input->shared || packet->fd_count != 1 ||
            packet->value != PH_USB_INPUT_SHARED_BYTES ||
            pacha_syscall2(PACHA_FD_SYSCALL_GET_INFO,
                packet->fds[0].fd, (uintptr_t)&info) ||
            info.kind != PACHA_FD_KIND_VMO ||
            info.size < PH_USB_INPUT_SHARED_BYTES ||
            (info.rights & (PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE)) !=
                (PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE))
            return -EPROTO;
        long mapped = pacha_syscall6(PACHA_VM_SYSCALL_MMAP,
            packet->fds[0].fd, 0, PH_USB_INPUT_SHARED_BYTES,
            PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
        if (mapped < 4096) return -ENOMEM;
        input->shared = (void *)(uintptr_t)mapped;
        input->shared_size = PH_USB_INPUT_SHARED_BYTES;
        break;
    }
    case PH_USB_INPUT_SNAPSHOT:
    case PH_USB_INPUT_READ:
        if (!input->shared || packet->fd_count || packet->value ||
            input->shared->magic != PH_USB_INPUT_SHARED_MAGIC ||
            input->shared->generation != input->generation)
            return -EPROTO;
        break;
    default:
        return -EPROTO;
    }
    input->operation = packet->operation;
    input->correlation = packet->correlation;
    return 0;
}

static int dispatch(void *context, void *linux_service) {
    struct ph_usb_input_service *input = context;
    struct ph_usb_input_shared *shared = input->shared;
    size_t count = 0;
    uint64_t cursor = 0;
    int result = 0;

    if (!shared || !input->operation) return -EPROTO;
    shared->operation = input->operation;
    shared->status = 0;
    shared->count = 0;
    shared->sequence = 0;
    shared->overwritten = 0;
    if (input->operation == PH_USB_INPUT_SNAPSHOT) {
        size_t capacity = (input->shared_size - offsetof(struct ph_usb_input_shared,
            payload)) / sizeof(struct kobox_linux_input_device_info);
        result = input->snapshot(linux_service,
            (struct kobox_linux_input_device_info *)shared->payload,
            capacity, &count, &cursor);
        shared->sequence = cursor;
    } else if (input->operation == PH_USB_INPUT_READ) {
        size_t capacity = (input->shared_size - offsetof(struct ph_usb_input_shared,
            payload)) / sizeof(struct kobox_linux_input_record);
        if (capacity > PH_USB_INPUT_READ_MAX) capacity = PH_USB_INPUT_READ_MAX;
        result = input->read(linux_service,
            (struct kobox_linux_input_record *)shared->payload,
            capacity, &count, &cursor);
        shared->overwritten = cursor;
    } else if (input->operation != PH_USB_INPUT_ATTACH) {
        return -EPROTO;
    }
    shared->status = result;
    shared->count = (uint32_t)count;
    atomic_thread_fence(memory_order_release);
    return 0;
}

static int complete(void *context, struct ph_ipc *ipc) {
    struct ph_usb_input_service *input = context;
    struct ph_ipc_packet reply = {.operation = input->operation,
        .generation = input->generation, .correlation = input->correlation,
        .value = (uint64_t)(int64_t)input->shared->status};
    return ph_ipc_send(ipc, &reply);
}

static int release(void *context) {
    struct ph_usb_input_service *input = context;
    input->operation = 0;
    input->correlation = 0;
    return 0;
}

static int stop(void *context) {
    struct ph_usb_input_service *input = context;
    if (!input->shared) return 0;
    if (pacha_syscall2(PACHA_VM_SYSCALL_MUNMAP,
            (uintptr_t)input->shared, input->shared_size)) return -EIO;
    input->shared = NULL;
    input->shared_size = 0;
    return 0;
}

int ph_usb_input_service_init(struct ph_usb_input_service *input, void *core,
    uint64_t generation, struct ph_lifecycle_service *service) {
    if (!input || !core || !generation || !service) return -EINVAL;
    void *read = ph_image_lookup(core, "kobox_linux_input_port_read");
    void *snapshot = ph_image_lookup(core, "kobox_linux_input_port_snapshot");
    if (!read || !snapshot) return -ENOENT;
    memset(input, 0, sizeof(*input));
    memcpy(&input->read, &read, sizeof(read));
    memcpy(&input->snapshot, &snapshot, sizeof(snapshot));
    input->generation = generation;
    *service = (struct ph_lifecycle_service){.context = input,
        .prepare = prepare, .dispatch = dispatch, .complete = complete,
        .release = release, .stop = stop};
    return 0;
}
