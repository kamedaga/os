#ifndef STORAGE_BOOT_BOOT_CONFIG_H
#define STORAGE_BOOT_BOOT_CONFIG_H

#include <stdint.h>

enum {
    STORAGE_BOOT_CONFIG_MAGIC = 0x3254424f4f544346ull,
    STORAGE_BOOT_CONFIG_VERSION = 2,
};

struct storage_boot_config {
    uint64_t magic;
    uint64_t version;
    uint64_t device_fd;
    uint64_t ready_channel_fd;
    uint64_t root_handoff_channel_fd;
    uint64_t bootfs_fd;
    uint64_t bootfs_size;
    uint64_t reserved[9];
};

_Static_assert(sizeof(struct storage_boot_config) == 128,
    "storage_boot private bootstrap ABI");

#endif
