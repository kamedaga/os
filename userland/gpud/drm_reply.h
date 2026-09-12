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
int gpud_drm_ioctl_reply(drmd_ioctl_request_t *request,
    const struct gpud_drm_translation *translation, uint64_t correlation,
    const unsigned char *reply, size_t reply_size,
    const unsigned char *output, size_t output_size);

#endif
