/* SPDX-License-Identifier: MIT */
#ifndef PACHA_INPUTD_SOURCE_PROTOCOL_H
#define PACHA_INPUTD_SOURCE_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include "boot/input_port.h"

enum {
    INPUTD_SOURCE_PAGE_BYTES = 4096,
    INPUTD_SOURCE_RECORD_MAX = 64,
    INPUTD_SOURCE_SYNC_BEGIN = 1,
    INPUTD_SOURCE_DEVICE = 2,
    INPUTD_SOURCE_SYNC_END = 3,
    INPUTD_SOURCE_EVENTS = 4,
};

#define INPUTD_SOURCE_MAGIC UINT64_C(0x314352535455504e)

/* A controller generation owns a source ID. A SYNC_BEGIN/DEVICE/SYNC_END
 * transaction reconciles devices without assuming one input node
 * per PCI function. Events carry the Kobox2 sequence and overflow counter. */
struct inputd_source_page {
    uint64_t magic;
    uint64_t source_id;
    uint64_t generation;
    uint64_t sequence;
    uint64_t overwritten;
    uint32_t kind;
    uint32_t count;
    uint16_t pci_segment;
    uint8_t pci_bus, pci_device, pci_function;
    uint8_t reserved[3];
    unsigned char payload[];
};

_Static_assert(offsetof(struct inputd_source_page, payload) +
    INPUTD_SOURCE_RECORD_MAX * sizeof(struct kobox_linux_input_record) <=
    INPUTD_SOURCE_PAGE_BYTES, "source events fit page");
_Static_assert(offsetof(struct inputd_source_page, payload) +
    sizeof(struct kobox_linux_input_device_info) <=
    INPUTD_SOURCE_PAGE_BYTES, "source device fits page");

#endif
