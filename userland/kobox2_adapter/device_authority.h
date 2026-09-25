/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_DEVICE_AUTHORITY_H
#define PACHA_KOBOX_DEVICE_AUTHORITY_H

#include <pacha/abi.h>
#include <stdint.h>

/* PachaOS native PCI-function profile. The function capability authorizes
 * config/MMIO, its isolated DMA domain, and deriving its interrupt routes.
 * These routes are NOT pre-bound kobox2.irq-endpoint resource objects. No
 * transferable MMIO/DMA alias or further delegation is granted to the child.
 * This native rights mask is not a kobox2 resource-rights wire value. */
#define PH_DEVICE_FUNCTION_RIGHTS (PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE | \
    PACHA_FD_RIGHT_QUERY | PACHA_FD_RIGHT_CONFIG_READ | PACHA_FD_RIGHT_CONFIG_WRITE | \
    PACHA_FD_RIGHT_DERIVE_MMIO | PACHA_FD_RIGHT_MMIO_MAP_READ | PACHA_FD_RIGHT_MMIO_MAP_WRITE | \
    PACHA_FD_RIGHT_DERIVE_DMA | PACHA_FD_RIGHT_DMA_READ | PACHA_FD_RIGHT_DMA_WRITE | \
    PACHA_FD_RIGHT_BUS_MASTER | PACHA_FD_RIGHT_DERIVE_IRQ | PACHA_FD_RIGHT_IRQ_WAIT | PACHA_FD_RIGHT_IRQ_ACK)

/* Independent native launch-owner policy. native_device comes from the
 * parent's queried capability, not peer claims. It is an OS-local identity,
 * never a GPU wire object ID. CAPSULE_QUERY.object_id is not usable for device
 * identity; the grant's generation/object_id supplies the logical identity. */
struct ph_device_authority {
    uint64_t generation, object_id, native_device;
    uint32_t slot_id, node_id;
};

#endif
