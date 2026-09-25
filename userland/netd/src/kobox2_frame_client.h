/* SPDX-License-Identifier: MIT */
#ifndef PACHA_NETD_KOBOX2_FRAME_CLIENT_H
#define PACHA_NETD_KOBOX2_FRAME_CLIENT_H

#include "../../kobox2_adapter/ipc.h"
#include "../../kobox2_adapter/net_frame_wire.h"

struct netd_kobox2_frame_client {
    struct ph_net_frame_shared *shared;
    uint64_t generation, next_correlation;
    int vmo_fd;
};

int netd_kobox2_frame_open(struct netd_kobox2_frame_client *client,
    struct ph_ipc *ipc, int process_fd, uint64_t generation);
int netd_kobox2_frame_info(struct netd_kobox2_frame_client *client,
    struct ph_ipc *ipc, int process_fd, struct kobox_linux_net_info *info);
int netd_kobox2_frame_read(struct netd_kobox2_frame_client *client,
    struct ph_ipc *ipc, int process_fd, const struct kobox_linux_net_frame **frames,
    size_t *count);
int netd_kobox2_frame_write(struct netd_kobox2_frame_client *client,
    struct ph_ipc *ipc, int process_fd, const void *frame, size_t length);
int netd_kobox2_frame_close(struct netd_kobox2_frame_client *client);

#endif
