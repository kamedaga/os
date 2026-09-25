/* SPDX-License-Identifier: MIT */
#ifndef PACHA_NETD_LINK_H
#define PACHA_NETD_LINK_H

#include <stddef.h>
#include <stdint.h>

enum { NETD_LINK_FRAME_MAX = 2048, NETD_LINK_ID_PRIMARY = 1 };

struct netd_link_info {
    uint64_t generation;
    uint32_t interface_id;
    uint32_t mtu;
    uint8_t mac[6];
    uint8_t carrier;
};

struct netd_link_ops {
    int (*send)(void *context, const void *frame, size_t length);
    void (*poll)(void *context);
};

typedef int (*netd_link_receive_fn)(void *context, uint64_t generation,
    const void *frame, size_t length);

/* netd's event loop owns these calls. The NIC producer may report RX only on
 * that loop; a cross-process producer must use its bounded transport first.
 * Detach invalidates old-generation callbacks before a replacement attaches. */
void netd_link_init(netd_link_receive_fn receive, void *context);
int netd_link_attach(const struct netd_link_info *info,
    const struct netd_link_ops *ops, void *context);
int netd_link_set_carrier(uint64_t generation, int carrier);
int netd_link_detach(uint64_t generation);
int netd_link_receive(uint64_t generation, const void *frame, size_t length);
int netd_link_send(const void *frame, size_t length);
void netd_link_poll(void);
const struct netd_link_info *netd_link_current(void);

#endif
