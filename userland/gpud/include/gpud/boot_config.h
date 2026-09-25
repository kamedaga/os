/* SPDX-License-Identifier: MIT */
#ifndef PACHA_GPUD_BOOT_CONFIG_H
#define PACHA_GPUD_BOOT_CONFIG_H

#include <stdint.h>

enum {
    GPUD_BOOT_CONFIG_MAGIC = 0x47505544424f4f54ull,
    GPUD_BOOT_CONFIG_VERSION = 2,
    GPUD_BOOT_READY_MAGIC = 0x3159445244555047ull,
    GPUD_CONTROL_FORCE_RESTART_MAGIC = 0x5453524655504743ull,
    GPUD_CONTROL_RESTARTED_MAGIC = 0x4454535255504743ull,
};

struct gpud_boot_config {
    uint64_t magic, version;
    uint64_t drm_endpoint_fd, device_fd, filed_endpoint_fd;
    uint64_t control_channel_fd, ready_channel_fd;
    uint64_t reserved[9];
};

_Static_assert(sizeof(struct gpud_boot_config) == 128, "gpud private bootstrap config");

#endif
