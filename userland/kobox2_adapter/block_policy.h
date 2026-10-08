/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_BLOCK_POLICY_H
#define PACHA_KOBOX_BLOCK_POLICY_H

#include <stdint.h>

#define PH_BLOCK_POLICY_ADDRESS UINT64_C(0x49000000)
#define PH_BLOCK_POLICY_MAGIC UINT64_C(0x314c4f504b4c4250)

/* A controller-owned, immutable launch blob. The source PCI capability is
 * already bound by the package identity; one whole-disk key additionally
 * authorizes writes for exactly this sandbox generation. */
struct ph_block_write_policy {
    uint64_t magic;
    uint64_t generation;
    uint64_t native_device;
    uint64_t enabled;
    uint64_t diskseq;
    uint32_t major;
    uint32_t minor;
};

#endif
