/* SPDX-License-Identifier: MIT */
#ifndef NETD_WIFI_FILES_H
#define NETD_WIFI_FILES_H
#include <pacha/abi.h>
#include <stddef.h>
#define NETD_WIFI_DIRECTORY "/run/pachaos/wifi"
#define NETD_WIFI_CONFIG "/etc/pachaos/wifi.conf"
int netd_wifi_files_prepare(pacha_fd_t endpoint);
int netd_wifi_file_read(pacha_fd_t endpoint, const char *path,
    void *bytes, size_t capacity, size_t *length);
int netd_wifi_file_write(pacha_fd_t endpoint, const char *name,
    const void *bytes, size_t length);
#endif
