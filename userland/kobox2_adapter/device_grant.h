/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_DEVICE_GRANT_H
#define PACHA_KOBOX_DEVICE_GRANT_H

#include "device_authority.h"
#include "package.h"

struct ph_device_grant {
    uint64_t generation, object_id, native_device;
    int fd;
};

/* After complete package verification, before any Linux resource import.
 * Checks the selected node binding, PCI-function schema, logical rights and
 * independent native identity; then checks actual FD/capsule rights, kind,
 * CLOEXEC and a translated/non-quarantined DMA domain. No device operation is
 * started. Success moves one staging FD to out, failure moves nothing.
 * Caller must validate all other resources before initializing any port.
 * Out is zeroed initially, cannot be overwritten while owning an FD, and
 * retains its generation watermark after close. Single owner. */
int ph_device_grant_take(struct ph_device_grant *out, struct ph_bootstrap_receiver *bundle,
    const struct ph_package *package, const struct ph_device_authority *authority);
/* Only after all Linux users/derived MMIO/DMA/IRQ ports have been stopped and
 * destroyed. Closing this handle alone is not device revoke/reset. Failed
 * close retains ownership for retry. */
int ph_device_grant_close(struct ph_device_grant *grant, uint64_t generation);

#endif
