#ifndef NETD_BOOT_CONFIG_H
#define NETD_BOOT_CONFIG_H

#include <stdint.h>

enum {
    NETD_BOOT_CONFIG_MAGIC = 0x4e455444424f4f54ull,
    NETD_BOOT_CONFIG_VERSION = 1,
    NETD_BOOT_CONFIG_VA = 0x3c100000ull,
    NETD_MODULE_IMAGE_VA = 0x50000000ull,
    NETD_MODULE_IMAGE_STRIDE = 0x01000000ull,
    NETD_MAX_MODULES = 8,
    NETD_BOOT_FLAG_SMOKE = 1ull << 0,
    NETD_BOOT_FLAG_TRACE = 1ull << 1,
    NETD_BOOT_FLAG_METRIC = 1ull << 2,
};

struct netd_module_config {
    uint64_t image_va;
    uint64_t image_size;
    char name[64];
};

struct netd_boot_config {
    uint64_t magic;
    uint64_t version;
    uint64_t device_fd;
    uint64_t module_count;
    struct netd_module_config modules[NETD_MAX_MODULES];
    uint64_t flags;
    uint64_t reserved[7];
};

#endif
