/* SPDX-License-Identifier: MIT */
#ifndef PACHA_GPUD_DRM_REPLY_H
#define PACHA_GPUD_DRM_REPLY_H

#include "drm_translate.h"

int gpud_drm_status_errno(uint32_t status);

/* Complete a saved private LPR ioctl request from private response snapshots.
 * Checks envelope generation/correlation, session and the exact result record
 * before updating native ABI output fields. Error returns leave request intact.
 * Does not consume/close FD attachments; this query path supports none.
 */
int gpud_drm_ioctl_reply(gpud_drm_ioctl_request_t *request,
    struct gpud_drm_translation *translation, uint64_t correlation,
    const unsigned char *reply, size_t reply_size,
    const unsigned char *output, size_t output_size);
int gpud_drm_poll_reply(uint32_t requested, uint32_t *ready,
    const struct gpud_drm_translation *translation, uint64_t correlation,
    const unsigned char *reply, size_t reply_size);
int gpud_drm_read_reply(gpud_drm_read_request_t *request,
    const struct gpud_drm_translation *translation, uint64_t correlation,
    const unsigned char *reply, size_t reply_size,
    const unsigned char *output, size_t output_size);
int gpud_drm_prime_export_reply(const struct gpud_drm_translation *translation,
    uint32_t handle, uint32_t flags, uint64_t correlation,
    const unsigned char *reply, size_t reply_size, uint64_t *token);
int gpud_drm_prime_import_reply(const struct gpud_drm_translation *translation,
    uint64_t token, uint64_t correlation,
    const unsigned char *reply, size_t reply_size, uint32_t *handle);

#endif
