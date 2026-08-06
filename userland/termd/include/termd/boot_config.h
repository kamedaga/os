#ifndef TERMD_BOOT_CONFIG_H
#define TERMD_BOOT_CONFIG_H

#include <stdint.h>

enum {
    TERMD_BOOT_CONFIG_MAGIC = 0x5445524d44424f4full,
    TERMD_BOOT_CONFIG_VERSION = 2,
    TERMD_MAX_MODULES = 8,
    TERMD_BOOT_FLAG_TRACE = 1ull << 0,
    TERMD_BOOT_READY_MAGIC = 0x31594452544d5254ull,
};

struct termd_boot_config {
    uint64_t magic;
    uint64_t version;
    uint64_t tty_endpoint_fd;
    uint64_t device_fd;
    uint64_t ready_channel_fd;
    uint64_t flags;
    uint64_t reserved[9];
};

_Static_assert(sizeof(struct termd_boot_config) == 120,
    "termd private bootstrap ABI");

#endif
