#pragma once

#include "drm_island.h"

#include <pacha/ipc.h>

typedef struct drmd_service {
    const struct drmd_boot_config *cfg;
    struct drmd_drm_island *drm;
    struct {
        int active;
        int page_fd;
        int reply_fd;
        void *page;
        pacha_service_envelope_t header;
    } deferred;
} drmd_service_t;

int drmd_service_send_boot_ready(drmd_service_t *service, int64_t status, uint64_t result);
int drmd_service_dispatch(
    drmd_service_t *service,
    const struct pacha_ipc_msg *request,
    const struct pacha_ipc_fd *fds);
int drmd_service_progress(drmd_service_t *service);
