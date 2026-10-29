/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_PLATFORM_AUTHORITY_H
#define PACHA_KOBOX_PLATFORM_AUTHORITY_H
#include <pacha/abi.h>
#include <stdint.h>

/* Controller writes include commands with reset effects. This is an
 * explicit controller owner, not an input-events-only consumer. No DMA,
 * PCI configuration, arbitrary I/O range or further delegation is granted. */
#define PH_PLATFORM_CONTROLLER_RIGHTS (PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE | \
    PACHA_FD_RIGHT_QUERY | PACHA_FD_RIGHT_READ | PACHA_FD_RIGHT_WRITE | PACHA_FD_RIGHT_CONTROLLER_WRITE | \
    PACHA_FD_RIGHT_DERIVE_IRQ | PACHA_FD_RIGHT_IRQ_WAIT | PACHA_FD_RIGHT_IRQ_ACK)

struct ph_platform_authority {
    uint64_t generation, object_id, platform_id;
    uint32_t slot_id, node_id;
};
#endif
