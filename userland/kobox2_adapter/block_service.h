/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_BLOCK_SERVICE_H
#define PACHA_KOBOX_BLOCK_SERVICE_H

#include "lifecycle.h"
#include "block_ipc.h"
#include "boot/block_port.h"

struct ph_block_service {
    unsigned char *shared;
    uint64_t generation;
    uint64_t sequence;
    uint64_t correlation;
    uint32_t operation;
    int (*snapshot)(struct kobox_linux_block_port *, size_t,
        struct kobox_linux_block_info *, size_t, size_t *, size_t *);
    int (*read)(struct kobox_linux_block_port *,
        const struct kobox_linux_block_key *, uint64_t, void *, size_t);
    int (*write)(struct kobox_linux_block_port *,
        const struct kobox_linux_block_key *, uint64_t, const void *, size_t);
    int (*flush)(struct kobox_linux_block_port *,
        const struct kobox_linux_block_key *);
};

int ph_block_service_init(struct ph_block_service *block, void *core,
    uint64_t generation, struct ph_lifecycle_service *service);

#endif
