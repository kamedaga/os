#ifndef LPR_SUPERVISOR_BOOT_CONFIG_H
#define LPR_SUPERVISOR_BOOT_CONFIG_H

#include <stdint.h>

enum {
    LPRS_BOOT_CONFIG_MAGIC = 0x315446434c535250ull,
    LPRS_BOOT_CONFIG_FD = 246,
};

struct lprs_boot_config {
    uint64_t magic;
    uint64_t endpoint_fd;
    uint64_t flags;
    uint64_t reserved[13];
};

#endif
