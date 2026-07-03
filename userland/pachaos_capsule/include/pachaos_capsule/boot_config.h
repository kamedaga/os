#ifndef PACHAOS_CAPSULE_BOOT_CONFIG_H
#define PACHAOS_CAPSULE_BOOT_CONFIG_H

#include <stdint.h>

enum {
    PACHAOS_CAPSULE_BOOT_CONFIG_MAGIC = 0x50434150424f4f54ull,
    PACHAOS_CAPSULE_BOOT_CONFIG_VERSION = 3,
    PACHAOS_CAPSULE_BOOT_CONFIG_VA = 0x3c100000ull,
    PACHAOS_CAPSULE_MODULE_IMAGE_VA = 0x50000000ull,
    PACHAOS_CAPSULE_MODULE_IMAGE_STRIDE = 0x01000000ull,
    PACHAOS_CAPSULE_MAX_MODULES = 8,
    PACHAOS_CAPSULE_WORKLOAD_NVME = 1,
    PACHAOS_CAPSULE_WORKLOAD_NET = 2,
};

struct pachaos_capsule_module_config {
    uint64_t image_va;
    uint64_t image_size;
    char name[64];
};

struct pachaos_capsule_boot_config {
    uint64_t magic;
    uint64_t version;
    uint64_t device_fd;
    uint64_t module_count;
    struct pachaos_capsule_module_config modules[PACHAOS_CAPSULE_MAX_MODULES];
    uint64_t workload;
    uint64_t reserved[7];
};

#endif
