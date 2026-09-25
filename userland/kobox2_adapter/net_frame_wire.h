/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_NET_FRAME_WIRE_H
#define PACHA_KOBOX_NET_FRAME_WIRE_H

#include <stddef.h>
#include <stdint.h>

#include "boot/net_port.h"

enum {
    PH_NET_FRAME_ATTACH = 0x300,
    PH_NET_FRAME_INFO = 0x301,
    PH_NET_FRAME_READ = 0x302,
    PH_NET_FRAME_WRITE = 0x303,
    PH_NET_FRAME_BATCH = 16,
    PH_NET_FRAME_SHARED_BYTES = 64 * 1024,
};

#define PH_NET_FRAME_SHARED_MAGIC UINT64_C(0x3154454e4f424f4b)

/* Exactly one management request owns this VMO until its correlated reply.
 * Linux pointers and native handles never enter the payload. The frame queue
 * in the hosted core remains bounded independently of this staging batch. */
struct ph_net_frame_shared {
    uint64_t magic;
    uint64_t generation;
    uint64_t operation;
    int32_t status;
    uint32_t count;
    struct kobox_linux_net_info info;
    struct kobox_linux_net_frame frames[PH_NET_FRAME_BATCH];
};

_Static_assert(sizeof(struct ph_net_frame_shared) <= PH_NET_FRAME_SHARED_BYTES,
    "network frame staging exceeds one VMO");

#endif
