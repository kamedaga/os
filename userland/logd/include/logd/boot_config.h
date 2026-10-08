/* SPDX-License-Identifier: MIT */
#ifndef PACHA_LOGD_BOOT_CONFIG_H
#define PACHA_LOGD_BOOT_CONFIG_H

#include <stdint.h>

#define LOGD_BOOT_MAGIC UINT64_C(0x31544f4f42474f4c)
#define LOGD_READY_MAGIC UINT64_C(0x31594452474f4c50)

struct logd_boot_config {
    uint64_t magic;
    uint64_t size;
    uint64_t endpoint_fd;
    uint64_t filed_endpoint_fd;
    uint64_t ready_channel_fd;
};

#endif
