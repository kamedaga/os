/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_NET_FRAME_SERVICE_H
#define PACHA_KOBOX_NET_FRAME_SERVICE_H

#include "lifecycle.h"
#include "net_frame_wire.h"

struct ph_net_frame_service {
    struct ph_net_frame_shared *shared;
    uint64_t generation;
    uint64_t operation;
    uint64_t correlation;
    int (*info)(void *, struct kobox_linux_net_info *);
    int (*read)(void *, struct kobox_linux_net_frame *, size_t, size_t *);
    int (*write)(void *, const void *, size_t);
};

int ph_net_frame_service_init(struct ph_net_frame_service *net, void *core,
    uint64_t generation, struct ph_lifecycle_service *service);

#endif
