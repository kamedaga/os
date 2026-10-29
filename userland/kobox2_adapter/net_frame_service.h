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
    int (*wifi_request)(void *, const void *, size_t, void *, size_t, size_t *);
    int (*wifi_configure)(void *, const void *, size_t);
    int (*wifi_events)(void *, uint32_t *);
    /* The peer can still write its VMO. Borrow no input from it after
     * prepare; lengths and bytes form one bounded private request snapshot. */
    uint32_t frame_count, wifi_size;
    struct kobox_linux_net_frame frame_snapshot[PH_NET_FRAME_BATCH];
    uint8_t wifi_snapshot[PH_NET_WIFI_BYTES];
};

int ph_net_frame_service_init(struct ph_net_frame_service *net, void *core,
    uint64_t generation, struct ph_lifecycle_service *service);

#endif
