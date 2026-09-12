/* SPDX-License-Identifier: MIT */
#ifndef PACHA_GPUD_DRM_SERVICE_H
#define PACHA_GPUD_DRM_SERVICE_H

#include "drm_files.h"
#include "gpu_rpc.h"

struct gpud_drm_watch {
    uint64_t handle;
    int fd;
    unsigned int transferred;
};

/* filed supplies one process-lifetime endpoint. Each OPEN/DUP transfers a
 * notification capability that owns one frontend reference; request payloads
 * never supply lifetime authority. Call only after sandbox READY and channel
 * binding. No Linux ABI enters the sandbox. */
struct gpud_drm_service {
    struct gpud_drm_files files;
    struct gpud_gpu_rpc gpu;
    uint64_t backend_client, correlation;
    struct gpud_drm_watch watches[GPUD_DRM_REFERENCES_MAX];
    /* Received capabilities and mappings remain owned here if cleanup fails.
     * The launch owner must retire the process, not reuse failed state. */
    struct ph_ipc_packet received;
    void *page;
    int endpoint_fd, error;
};

/* Binds this generation to filed's process-lifetime DRM endpoint. The
 * endpoint is borrowed so a replacement generation can bind it again. */
int gpud_drm_service_bind(struct gpud_drm_service *service, int endpoint_fd);
/* Receives and replies to one existing LPR native IPC_CALL. -EAGAIN means no
 * request; other errors stop the service. A Linux operation error is sent in
 * the reply, not returned here. CLOSE waits for the real backend completion. */
int gpud_drm_service_receive(struct gpud_drm_service *service);
/* Transfer leases and the original open notification each own one frontend
 * reference. HANGUP drops only that reference and closes the backend session
 * synchronously when it was the last one. */
int gpud_drm_service_reap_hangups(struct gpud_drm_service *service);
size_t gpud_drm_service_collect_wait_sources(
    const struct gpud_drm_service *service, int *fds, size_t capacity);

#endif
