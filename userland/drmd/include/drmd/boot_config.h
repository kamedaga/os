#ifndef DRMD_BOOT_CONFIG_H
#define DRMD_BOOT_CONFIG_H

#include <stdint.h>

enum {
    DRMD_BOOT_CONFIG_MAGIC = 0x44524d44424f4f55ull,
    DRMD_BOOT_CONFIG_VA = 0x3c190000ull,
    DRMD_MODULE_IMAGE_VA = 0x62000000ull,
    DRMD_MODULE_IMAGE_STRIDE = 0x01000000ull,
    DRMD_MAX_MODULES = 7,
    DRMD_BOOT_READY_MAGIC = 0x31594452444d5244ull,
};

struct drmd_module_config {
    uint64_t image_va;
    uint64_t image_size;
    char name[64];
};

struct drmd_boot_config {
    uint64_t magic;
    uint64_t drm_endpoint_fd;
    uint64_t device_fd;
    uint64_t ready_channel_fd;
    uint64_t netd_endpoint_fd;
    uint64_t module_count;
    struct drmd_module_config modules[DRMD_MAX_MODULES];
    uint64_t reserved[5];
};

#endif
