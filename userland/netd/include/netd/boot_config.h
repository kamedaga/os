#ifndef NETD_BOOT_CONFIG_H
#define NETD_BOOT_CONFIG_H

#include <stdint.h>

enum {
    NETD_BOOT_CONFIG_MAGIC = 0x4e455444424f4f54ull,
    NETD_BOOT_CONFIG_VERSION = 2,
    NETD_MAX_MODULES = 8,
    NETD_BOOT_FLAG_SMOKE = 1ull << 0,
    NETD_BOOT_FLAG_TRACE = 1ull << 1,
    NETD_BOOT_FLAG_METRIC = 1ull << 2,
};

struct netd_boot_config {
    uint64_t magic;
    uint64_t version;
    uint64_t device_fd;
    uint64_t socket_endpoint_fd;
    uint64_t flags;
    uint64_t reserved[10];
};

_Static_assert(sizeof(struct netd_boot_config) == 120,
    "netd private bootstrap ABI");

#endif
