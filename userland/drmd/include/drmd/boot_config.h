#ifndef DRMD_BOOT_CONFIG_H
#define DRMD_BOOT_CONFIG_H

#include <stdint.h>

enum {
    DRMD_BOOT_CONFIG_MAGIC = 0x44524d44424f4f55ull,
    DRMD_BOOT_CONFIG_VERSION = 2,
    DRMD_MAX_MODULES = 7,
    DRMD_BOOT_READY_MAGIC = 0x31594452444d5244ull,
};

struct drmd_boot_config {
    uint64_t magic;
    uint64_t version;
    uint64_t drm_endpoint_fd;
    uint64_t device_fd;
    uint64_t ready_channel_fd;
    uint64_t netd_endpoint_fd;
    uint64_t reserved[10];
};

_Static_assert(sizeof(struct drmd_boot_config) == 128,
    "drmd private bootstrap ABI");

#endif
