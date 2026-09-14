/* SPDX-License-Identifier: MIT */
#ifndef PACHA_GPUD_DRM_TRANSLATE_H
#define PACHA_GPUD_DRM_TRANSLATE_H

#include <gpud/drm_protocol.h>
#include <kobox2/gpu.h>

#define GPUD_DRM_COMMAND_BYTES 1024u
#define GPUD_DRM_VERSION_BYTES \
    (GPUD_DRM_VERSION_NAME_BYTES + GPUD_DRM_VERSION_DATE_BYTES + GPUD_DRM_VERSION_DESC_BYTES)
#define GPUD_DRM_STAGED_INPUT_BYTES \
    (GPUD_DRM_KMS_CONNECTOR_CAPACITY * sizeof(uint32_t) + \
     sizeof(gpud_drm_modeinfo_t))

/* Resolved by the authenticated gpud session owner, not copied from a peer.
 * The frontend handle identifies an LPR open-file description. session_id is
 * its independently resolved sandbox session in this generation. */
struct gpud_drm_binding {
    uint64_t generation, frontend_handle, session_id;
};

struct gpud_drm_translation {
    uint64_t generation, frontend_handle, session_id;
    uint32_t command_set_id, command_id, queue_class;
    size_t command_size;
    unsigned char command[GPUD_DRM_COMMAND_BYTES];
    /* Independently registered command data. VERSION and GET_CAPS write it;
     * EXECBUFFER and CONTEXT_INIT read it. No local pointer is serialized. */
    kb2_gpu_region_t region;
    uint32_t version_capacity[3];
    uint32_t output_capacity[4], output_offset[4];
    size_t staged_input_size;
    unsigned char staged_input[GPUD_DRM_STAGED_INPUT_BYTES];
    uint64_t mapping_id, mapping_length, mapping_exchange;
    uint32_t mapping_rights, mapping_cache_policy;
};

/* Frontend only; never link this gpud DRM service ABI into the GPL core.
 * request must already be a complete, privately owned native IPC snapshot.
 * Unsupported operations return EOPNOTSUPP, never forward raw ioctl bytes.
 * Bindings, attachment ownership and region registration are the caller's
 * responsibility. VERSION with any nonzero capacity needs a registered
 * region_id; a pure VERSION length query needs no region.
 * No allocation, syscall or retained input pointer. Failure leaves out intact.
 * This constructs a request; it does not publish it or claim device support. */
int gpud_drm_ioctl_encode(struct gpud_drm_translation *out,
    const struct gpud_drm_binding *binding, const gpud_drm_ioctl_request_t *request,
    uint32_t region_id);
int gpud_drm_prime_export_encode(struct gpud_drm_translation *out,
    const struct gpud_drm_binding *binding, uint32_t handle, uint32_t flags);
int gpud_drm_prime_import_encode(struct gpud_drm_translation *out,
    const struct gpud_drm_binding *binding, uint64_t token);
int gpud_drm_poll_encode(struct gpud_drm_translation *out,
    const struct gpud_drm_binding *binding, uint32_t events);
int gpud_drm_read_encode(struct gpud_drm_translation *out,
    const struct gpud_drm_binding *binding, uint32_t capacity,
    uint32_t region_id);

#endif
