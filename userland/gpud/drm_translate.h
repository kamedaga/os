/* SPDX-License-Identifier: MIT */
#ifndef PACHA_GPUD_DRM_TRANSLATE_H
#define PACHA_GPUD_DRM_TRANSLATE_H

#include <drmd/ipc_protocol.h>
#include <kobox2/gpu.h>

#define GPUD_DRM_COMMAND_BYTES 1024u
#define GPUD_DRM_VERSION_BYTES \
    (DRMD_VERSION_NAME_BYTES + DRMD_VERSION_DATE_BYTES + DRMD_VERSION_DESC_BYTES)

/* Resolved by the authenticated gpud session owner, not copied from a peer.
 * The frontend handle identifies an LPR open-file description. session_id is
 * its independently resolved sandbox session in this generation. */
struct gpud_drm_binding {
    uint64_t generation, frontend_handle, session_id;
};

struct gpud_drm_translation {
    uint64_t generation, frontend_handle, session_id;
    uint32_t command_id, queue_class;
    size_t command_size;
    unsigned char command[GPUD_DRM_COMMAND_BYTES];
    /* VERSION uses one independently registered output region, with spans at
     * fixed offsets 0, NAME_BYTES and NAME_BYTES + DATE_BYTES. Other supported
     * commands have no external region. No local pointer is serialized. */
    kb2_gpu_region_t output_region;
    uint32_t version_capacity[3];
};

/* Frontend only; never link this drmd ABI dependency into the GPL core.
 * request must already be a complete, privately owned native IPC snapshot.
 * Unsupported operations return EOPNOTSUPP, never forward raw ioctl bytes.
 * Bindings, attachment ownership and region registration are the caller's
 * responsibility. VERSION with any nonzero capacity needs a registered
 * output_region_id; a pure length query needs no region.
 * No allocation, syscall or retained input pointer. Failure leaves out intact.
 * This constructs a request; it does not publish it or claim device support. */
int gpud_drm_ioctl_encode(struct gpud_drm_translation *out,
    const struct gpud_drm_binding *binding, const drmd_ioctl_request_t *request,
    uint32_t output_region_id);

#endif
