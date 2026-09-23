/* SPDX-License-Identifier: MIT */
#ifndef PACHA_GPUD_DRM_FILES_H
#define PACHA_GPUD_DRM_FILES_H

#include "drm_translate.h"
#include "gpu_limits.h"

enum { GPUD_DRM_FILES_MAX = GPUD_GPU_NATIVE_SESSION_LIMIT, GPUD_DRM_REFERENCES_MAX = 64 };
enum gpud_drm_file_state {
    GPUD_DRM_FILE_FREE,
    GPUD_DRM_FILE_OPENING,
    GPUD_DRM_FILE_OPEN,
    GPUD_DRM_FILE_DRAINING,
    GPUD_DRM_FILE_CLOSING,
    GPUD_DRM_FILE_FAILED,
};

struct gpud_drm_file {
    uint64_t handle, session;
    uint32_t references, in_flight;
    enum gpud_drm_file_state state;
};

/* Frontend only. One gpud owner serializes these calls with transport
 * completions. A handle denotes an open-file description: DUP and FD transfer
 * preserve both the handle and backend session and add one reference. The
 * corresponding notification capability is owned by drm_service and removes
 * that reference on explicit close or HANGUP.
 *
 * OPENING and CLOSING retain ownership across transport errors. After a
 * terminal error this table is not reset or reused; the generation is retired.
 */
struct gpud_drm_files {
    uint64_t generation, handle_sequence;
    size_t limit;
    int terminal_error;
    struct gpud_drm_file files[GPUD_DRM_FILES_MAX];
};

int gpud_drm_files_init(struct gpud_drm_files *files, uint64_t generation, size_t limit);
/* Reserve before sending SESSION_OPEN. handle is private until open_finish
 * succeeds; it must not be returned to LPR on an uncertain transport result.
 */
int gpud_drm_file_open_begin(struct gpud_drm_files *files,
                             uint64_t generation,
                             uint64_t *handle_out);
int gpud_drm_file_open_finish(struct gpud_drm_files *files,
                              uint64_t generation,
                              uint64_t handle,
                              uint64_t session,
                              int error);
int gpud_drm_file_dup(struct gpud_drm_files *files,
                      uint64_t generation,
                      uint64_t handle);
/* Drops one frontend reference. On the last reference, the service must
 * retain the LPR CLOSE reply until next_close/control_complete confirms the
 * backend result; this local transition is not a successful CLOSE reply. */
int gpud_drm_file_close(struct gpud_drm_files *files,
                        uint64_t generation,
                        uint64_t handle);
int gpud_drm_file_acquire(struct gpud_drm_files *files,
                          uint64_t generation,
                          uint64_t handle,
                          struct gpud_drm_binding *binding_out);
/* Release the private admitted request after its backend completion. */
int gpud_drm_file_release(struct gpud_drm_files *files, uint64_t generation, uint64_t handle);
/* Returns 1 with a claimed close, 0 if none is ready, or a negative error.
 * A close is not issued while accepted ioctls still hold the description.
 */
int gpud_drm_file_next_close(struct gpud_drm_files *files,
                             uint64_t generation,
                             struct gpud_drm_binding *binding_out);
int gpud_drm_file_close_finish(struct gpud_drm_files *files,
                               uint64_t generation,
                               uint64_t handle,
                               int error);
/* Unknown send/receive outcome is terminal, not a failed OPEN rollback. */
int gpud_drm_files_fault(struct gpud_drm_files *files, uint64_t generation, int error);

#endif
