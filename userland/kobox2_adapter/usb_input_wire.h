/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_USB_INPUT_WIRE_H
#define PACHA_KOBOX_USB_INPUT_WIRE_H

#include <stddef.h>
#include <stdint.h>

#include "boot/input_port.h"

enum {
    PH_USB_INPUT_ATTACH = 0x200,
    PH_USB_INPUT_SNAPSHOT = 0x201,
    PH_USB_INPUT_READ = 0x202,
    PH_USB_INPUT_SHARED_BYTES = 64 * 1024,
    PH_USB_INPUT_READ_MAX = 64,
};

#define PH_USB_INPUT_SHARED_MAGIC UINT64_C(0x315455504e495542)

/* One management request owns this buffer until its matching reply. The
 * sandbox writes only after Linux has finished the operation; a sequence gap
 * or overwritten count obliges the consumer to take a fresh snapshot. */
struct ph_usb_input_shared {
    uint64_t magic;
    uint64_t generation;
    uint64_t operation;
    int32_t status;
    uint32_t count;
    uint64_t sequence;
    uint64_t overwritten;
    unsigned char payload[];
};

_Static_assert(offsetof(struct ph_usb_input_shared, payload) % 8 == 0,
    "USB input payload alignment");

#endif
