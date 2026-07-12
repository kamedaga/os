#ifndef INPUTD_BOOT_CONFIG_H
#define INPUTD_BOOT_CONFIG_H

#include <stdint.h>

enum {
    INPUTD_BOOT_CONFIG_MAGIC = 0x494e5054424f4f54ull,
    INPUTD_BOOT_CONFIG_VA = 0x3c1a0000ull,
    INPUTD_MODULE_IMAGE_VA = 0x69000000ull,
    INPUTD_MODULE_IMAGE_STRIDE = 0x01000000ull,
    INPUTD_MAX_MODULES = 6,
    INPUTD_DEVICE_COUNT = 2,
    INPUTD_BOOT_READY_MAGIC = 0x3159445254504e49ull,
};

struct inputd_module_config {
    uint64_t image_va;
    uint64_t image_size;
    char name[64];
};

struct inputd_boot_config {
    uint64_t magic;
    uint64_t input_endpoint_fd;
    uint64_t ready_channel_fd;
    uint64_t device_count;
    uint64_t device_fds[INPUTD_DEVICE_COUNT];
    uint64_t module_count;
    struct inputd_module_config modules[INPUTD_MAX_MODULES];
    uint64_t reserved[6];
};

#endif
