#include <pacha/abi.h>
/* SPDX-License-Identifier: MIT */
#ifndef PACHA_NETD_KOBOX2_FRAME_CLIENT_H
#define PACHA_NETD_KOBOX2_FRAME_CLIENT_H

#include "../../kobox2_adapter/ipc.h"
#include "../../kobox2_adapter/net_frame_wire.h"

struct netd_kobox2_frame_client {
    struct ph_net_frame_shared *shared;
    uint64_t generation, next_correlation;
    pacha_fd_t vmo_fd;
};

int netd_kobox2_frame_open(struct netd_kobox2_frame_client *client,
    struct ph_ipc *ipc, pacha_fd_t process_fd, uint64_t generation);
int netd_kobox2_frame_info(struct netd_kobox2_frame_client *client,
    struct ph_ipc *ipc, pacha_fd_t process_fd, struct kobox_linux_net_info *info);
int netd_kobox2_frame_read(struct netd_kobox2_frame_client *client,
    struct ph_ipc *ipc, pacha_fd_t process_fd, const struct kobox_linux_net_frame **frames,
    size_t *count);
int netd_kobox2_frame_write(struct netd_kobox2_frame_client *client,
    struct ph_ipc *ipc, pacha_fd_t process_fd, const void *frame, size_t length);
int netd_kobox2_wifi_request(struct netd_kobox2_frame_client *client,
    struct ph_ipc *ipc, pacha_fd_t process_fd, const void *command, size_t length,
    void *reply, size_t capacity, size_t *received);
int netd_kobox2_wifi_configure(struct netd_kobox2_frame_client *client,
    struct ph_ipc *ipc, pacha_fd_t process_fd, const void *configuration, size_t length);
int netd_kobox2_wifi_events(struct netd_kobox2_frame_client *client,
    struct ph_ipc *ipc, pacha_fd_t process_fd, uint32_t *events);
int netd_kobox2_frame_close(struct netd_kobox2_frame_client *client);

#endif
