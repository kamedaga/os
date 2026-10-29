#include <pacha/abi.h>
/* SPDX-License-Identifier: MIT */
#ifndef PACHA_USBD_SOURCE_CLIENT_H
#define PACHA_USBD_SOURCE_CLIENT_H

#include "input_bridge.h"
#include <inputd/source_protocol.h>

struct usbd_source_client {
    pacha_fd_t endpoint_fd;
    pacha_fd_t page_fd;
    struct inputd_source_page *page;
    uint64_t source_id, generation, correlation;
    uint64_t next_sequence, overwritten;
    uint64_t platform_id;
    uint32_t source_kind;
    uint16_t pci_segment;
    uint8_t pci_bus, pci_device, pci_function;
};

int usbd_source_open(struct usbd_source_client *source, pacha_fd_t endpoint_fd,
    uint64_t source_id, uint64_t generation,
    uint16_t pci_segment, uint8_t pci_bus, uint8_t pci_device,
    uint8_t pci_function);
int usbd_source_sync(struct usbd_source_client *source,
    struct usbd_input_bridge *bridge, struct ph_ipc *ipc, pacha_fd_t process_fd,
    int lost_events);
int usbd_source_pump(struct usbd_source_client *source,
    struct usbd_input_bridge *bridge, struct ph_ipc *ipc, pacha_fd_t process_fd);
int usbd_source_close(struct usbd_source_client *source);

#endif
