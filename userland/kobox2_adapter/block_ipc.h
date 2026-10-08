/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_BLOCK_IPC_H
#define PACHA_KOBOX_BLOCK_IPC_H

#include <kobox2/block_device.h>

/* Shared-memory operation IDs are private to the Kobox2 transport. The
 * serialized device schema is in kobox2/protocol, not in this header. */
enum {
    PH_BLOCK_ATTACH = 0x300,
    PH_BLOCK_SNAPSHOT = 0x301,
    PH_BLOCK_READ = 0x302,
    PH_BLOCK_WRITE = 0x303,
    PH_BLOCK_FLUSH = 0x304,
    PH_BLOCK_SHARED_BYTES = 4096 + KB2_BLOCK_DEVICE_MAX_TRANSFER,
};

#endif
