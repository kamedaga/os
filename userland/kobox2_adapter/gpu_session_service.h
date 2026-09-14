/* SPDX-License-Identifier: MIT */
#ifndef PACHA_GPU_SESSION_SERVICE_H
#define PACHA_GPU_SESSION_SERVICE_H

#include "../gpud/gpu_sessions.h"
#include "boot/drm_service.h"
#include "gpu_query.h"

/* Native-side policy and private plans. Only dispatch enters the GPL Linux
 * service. One lifecycle serializes prepare/dispatch/release across both
 * lanes. Additional channel owners must serialize access to the shared table
 * as well; the table does not supply locks. BUSY CLOSE retains CLOSING and
 * requires a retry after accepted work drains; it never acknowledges close. */
struct ph_gpu_session_service {
    struct ph_gpu_query query;
    struct ph_lifecycle_service query_service;
    struct gpud_gpu_sessions *sessions;
    uint64_t client_id, session_id, file_cookie, correlation, command_count;
    uint64_t last_control;
    uint32_t opcode, status, node_type;
    int acquired;
    int (*open)(struct kobox_linux_drm_service *, uint32_t, uint64_t *);
    int (*file)(struct kobox_linux_drm_service *, uint64_t, struct kobox_linux_drm_file **);
    int (*close)(struct kobox_linux_drm_service *, uint64_t);
    int (*unmap)(struct kobox_linux_drm_service *, uint64_t);
};

int ph_gpu_session_service_init(struct ph_gpu_session_service *service,
                                struct gpud_gpu_sessions *sessions,
                                uint64_t client_id);
/* query.request already contains the private complete envelope snapshot. */
int ph_gpu_session_prepare(struct ph_gpu_session_service *service,
                           uint32_t queue_class,
                           size_t size);
int ph_gpu_session_dispatch(struct ph_gpu_session_service *service, void *linux_service);
int ph_gpu_session_service_release(struct ph_gpu_session_service *service);

#endif
