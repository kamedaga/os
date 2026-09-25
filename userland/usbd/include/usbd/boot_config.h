/* SPDX-License-Identifier: MIT */
#ifndef PACHA_USBD_BOOT_CONFIG_H
#define PACHA_USBD_BOOT_CONFIG_H

#include <stdint.h>

enum {
    USBD_BOOT_CONFIG_MAGIC = 0x3147464344425355ull,
    USBD_BOOT_CONFIG_VERSION = 2,
    USBD_BOOT_READY_MAGIC = 0x3159445244425355ull,
};

/* Private seed0root -> usbd startup data. Resource rights are still carried
 * by explicit FD grants, not by these integer fields. */
struct usbd_boot_config {
    uint64_t magic;
    uint64_t version;
    uint64_t device_fd;
    uint64_t filed_endpoint_fd;
    uint64_t ready_channel_fd;
    uint64_t input_source_endpoint_fd;
    uint64_t resource_id;
    uint16_t pci_segment;
    uint8_t pci_bus, pci_device, pci_function;
    uint8_t reserved[3];
};

#endif
