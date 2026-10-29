/* SPDX-License-Identifier: MIT */
#ifndef PACHA_AUDIOD_BOOT_CONFIG_H
#define PACHA_AUDIOD_BOOT_CONFIG_H
#include <stdint.h>
#define AUDIOD_BOOT_MAGIC UINT64_C(0x415544494f424f54)
#define AUDIOD_READY_MAGIC UINT64_C(0x415544494f524459)
struct audiod_boot_config {
    uint64_t magic, size, device_fd, filed_fd, endpoint_fd, ready_fd;
};
#endif
