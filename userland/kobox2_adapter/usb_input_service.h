/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_USB_INPUT_SERVICE_H
#define PACHA_KOBOX_USB_INPUT_SERVICE_H

#include <pacha/abi.h>
#include "lifecycle.h"
#include "usb_input_wire.h"

struct ph_usb_input_service {
    struct ph_usb_input_shared *shared;
    size_t shared_size;
    uint64_t generation;
    uint64_t operation;
    uint64_t correlation;
    pacha_fd_t event_fd;
    struct kobox_linux_input_notify notify;
    int (*bind_notify)(void *, const struct kobox_linux_input_notify *);
    int (*read)(void *, struct kobox_linux_input_record *, size_t,
        size_t *, uint64_t *);
    int (*snapshot)(void *, struct kobox_linux_input_device_info *, size_t,
        size_t *, uint64_t *);
};

int ph_usb_input_service_init(struct ph_usb_input_service *input, void *core,
    uint64_t generation, struct ph_lifecycle_service *service);

#endif
