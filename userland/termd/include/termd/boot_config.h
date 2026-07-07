#ifndef TERMD_BOOT_CONFIG_H
#define TERMD_BOOT_CONFIG_H

#include <stdint.h>

enum {
    TERMD_BOOT_CONFIG_MAGIC = 0x5445524d44424f4full,
    TERMD_BOOT_CONFIG_VA = 0x3c180000ull,
    TERMD_MODULE_IMAGE_VA = 0x52000000ull,
    TERMD_MODULE_IMAGE_STRIDE = 0x01000000ull,
    TERMD_MAX_MODULES = 8,
    TERMD_BOOT_FLAG_TRACE = 1ull << 0,
    TERMD_BOOT_READY_MAGIC = 0x31594452544d5254ull,
};

struct termd_module_config {
    uint64_t image_va;
    uint64_t image_size;
    char name[64];
};

struct termd_boot_config {
    uint64_t magic;
    uint64_t tty_endpoint_fd;
    uint64_t device_fd;
    uint64_t ready_channel_fd;
    uint64_t module_count;
    struct termd_module_config modules[TERMD_MAX_MODULES];
    uint64_t flags;
    uint64_t reserved[5];
};

#endif
