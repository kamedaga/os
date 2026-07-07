#ifndef STORAGE_BOOT_BOOT_CONFIG_H
#define STORAGE_BOOT_BOOT_CONFIG_H

#include <stdint.h>

enum {
    STORAGE_BOOT_CONFIG_MAGIC = 0x5354424f4f544346ull,
    STORAGE_BOOT_MAX_MODULES = 8,
};

struct storage_boot_module_config {
    uint64_t image_fd;
    uint64_t image_size;
    char name[64];
};

struct storage_boot_config {
    uint64_t magic;
    uint64_t device_fd;
    uint64_t ready_channel_fd;
    uint64_t service_ready_channel_fd;
    uint64_t module_count;
    struct storage_boot_module_config modules[STORAGE_BOOT_MAX_MODULES];
    uint64_t reserved[6];
};

#endif
