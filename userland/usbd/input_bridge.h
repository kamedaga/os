/* SPDX-License-Identifier: MIT */
#ifndef PACHA_USBD_INPUT_BRIDGE_H
#define PACHA_USBD_INPUT_BRIDGE_H

#include "../kobox2_adapter/usb_input_wire.h"
#include "../kobox2_adapter/ipc.h"

struct usbd_input_bridge {
    int vmo_fd;
    struct ph_usb_input_shared *shared;
    uint64_t generation;
    uint64_t next_correlation;
};

int usbd_input_bridge_open(struct usbd_input_bridge *bridge,
    struct ph_ipc *ipc, int process_fd, uint64_t generation);
int usbd_input_bridge_snapshot(struct usbd_input_bridge *bridge,
    struct ph_ipc *ipc, int process_fd,
    const struct kobox_linux_input_device_info **devices,
    size_t *count, uint64_t *next_sequence);
int usbd_input_bridge_read(struct usbd_input_bridge *bridge,
    struct ph_ipc *ipc, int process_fd,
    const struct kobox_linux_input_record **records,
    size_t *count, uint64_t *overwritten);
int usbd_input_bridge_close(struct usbd_input_bridge *bridge);

#endif
