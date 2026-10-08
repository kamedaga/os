/* SPDX-License-Identifier: MIT */
#ifndef PACHA_GPUD_BOOT_CONFIG_H
#define PACHA_GPUD_BOOT_CONFIG_H

#include <stdint.h>

enum {
    GPUD_BOOT_CONFIG_MAGIC = 0x47505544424f4f54ull,
    GPUD_BACKEND_VIRTIO = 1,
    GPUD_BACKEND_PCI_DRM_INSPECT = 2,
    GPUD_BOOT_READY_MAGIC = 0x3159445244555047ull,
    GPUD_CONTROL_FORCE_RESTART_MAGIC = 0x5453524655504743ull,
    GPUD_CONTROL_STOP_MAGIC = 0x504f545355504743ull,
    GPUD_CONTROL_RESTARTED_MAGIC = 0x4454535255504743ull,
    /* Private owner handshake: release GOP's MMIO lease before PCI probe. */
    GPUD_CONTROL_DISPLAY_RELEASE_MAGIC = 0x5145524c50554447ull,
    GPUD_CONTROL_DISPLAY_RELEASED_MAGIC = 0x4445524c50554447ull,
};

struct gpud_boot_config {
    uint64_t magic, size;
    uint64_t drm_endpoint_fd, device_fd, filed_endpoint_fd;
    uint64_t control_channel_fd, ready_channel_fd;
    /* Private owner-to-gpud policy, never a PCI ID or sandbox-supplied hint. */
    uint64_t backend;
    char package_root[64];
    /* Optional SEND-only RAM log endpoint; zero preserves serial fallback. */
    uint64_t log_endpoint_fd;
    /* Supplied by the privileged package owner, never by the sandbox. */
    uint8_t firmware_sha256[32];
    /* Bind RAM-uploaded executable and closure to the trusted boot image. */
    uint8_t manifest_sha256[32];
    uint8_t sandbox_sha256[32];
};

_Static_assert(sizeof(struct gpud_boot_config) == 232, "gpud private bootstrap config");

#endif
