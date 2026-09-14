/* SPDX-License-Identifier: MIT */
#ifndef PACHA_GPUD_DRM_CONTROL_H
#define PACHA_GPUD_DRM_CONTROL_H

#include "drm_files.h"
#include <kobox2/gpu_session.h>

/* A controller-side transaction, never a wire object. Only a verified reply
 * consumes it. Unknown transport outcomes fault the generation and retain
 * the reserved description for sandbox retirement, not local rollback. */
struct gpud_drm_control {
    uint64_t generation, correlation, handle, session;
    uint32_t opcode;
};

/* Converts a private LPR OPEN_NODE payload and reserves frontend ownership.
 * backend_client is gpud's authenticated sandbox-channel binding, not the
 * originating LPR client. Initial profile supports render O_RDWR, NONBLOCK
 * and CLOEXEC; unsupported node/access/status flags are explicit errors.
 */
int gpud_drm_open_prepare(struct gpud_drm_files *files,
                          uint64_t generation,
                          uint64_t backend_client,
                          uint64_t correlation,
                          const gpud_drm_open_request_t *request,
                          struct gpud_drm_control *pending,
                          unsigned char *bytes,
                          size_t capacity,
                          size_t *size_out);
/* Claims a description whose last reference and accepted ioctl have left.
 * Returns 1 with a SESSION_CLOSE request, 0 if none is ready, or an error. */
int gpud_drm_close_prepare(struct gpud_drm_files *files,
                           uint64_t generation,
                           uint64_t correlation,
                           struct gpud_drm_control *pending,
                           unsigned char *bytes,
                           size_t capacity,
                           size_t *size_out);
int gpud_drm_control_complete(struct gpud_drm_files *files,
                              struct gpud_drm_control *pending,
                              const unsigned char *reply,
                              size_t size,
                              uint64_t *handle_out);

#endif
