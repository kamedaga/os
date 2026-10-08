/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_USB_STORAGE_SERVICE_H
#define PACHA_KOBOX_USB_STORAGE_SERVICE_H

#include "usb_input_service.h"
#include "block_service.h"

struct ph_usb_storage_service {
    struct ph_usb_input_service input;
    struct ph_block_service block;
    struct ph_lifecycle_service input_ops;
    struct ph_lifecycle_service block_ops;
    uint32_t active;
};

int ph_usb_storage_service_init(struct ph_usb_storage_service *state,
    void *core, uint64_t generation, struct ph_lifecycle_service *service);

#endif
