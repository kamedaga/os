#include "drmd/boot_config.h"
#include "drmd_service.h"

#include <pacha/abi.h>
#include <pacha/ipc.h>
#include <kobox/shim.h>

#include <stdio.h>
#include <string.h>

enum { DRMD_ACTIVE_GRACE_TICKS = 4 };

int main(void)
{
    const struct drmd_boot_config *cfg =
        (const struct drmd_boot_config *)(uintptr_t)DRMD_BOOT_CONFIG_VA;
    if (cfg == NULL || cfg->magic != DRMD_BOOT_CONFIG_MAGIC ||
        cfg->drm_endpoint_fd < 16 || cfg->device_fd < 16 ||
        cfg->ready_channel_fd < 16 || cfg->netd_endpoint_fd < 16) {
        return 1;
    }
    static struct drmd_drm_island island;
    drmd_service_t service = { .cfg = cfg, .drm = &island };
    const int init_status = drmd_drm_island_init(&island, cfg);
    printf("[drmd] init status=%d modules=%u card0=%s\n",
        init_status,
        island.loaded_module_count,
        island.ready ? "ready" : "missing");
    const int ready_status = drmd_service_send_boot_ready(
        &service,
        init_status,
        island.ready ? 1 : 0);
    if (ready_status != 0 || init_status != 0) {
        return 1;
    }
    unsigned active_grace_ticks = 0;
    for (;;) {
        static struct pacha_service_wait_set wait_set;
        if (pacha_service_wait_init(&wait_set, (int)cfg->drm_endpoint_fd) != 0)
            return 1;
        int notify_fds[DRMD_DRM_WAIT_SOURCE_MAX];
        const size_t notify_count = drmd_drm_island_collect_wait_sources(
            notify_fds, DRMD_DRM_WAIT_SOURCE_MAX);
        for (size_t i = 0; i < notify_count; i++) {
            if (pacha_service_wait_add(
                    &wait_set, notify_fds[i], PACHA_FD_EVENT_HANGUP) != 0)
                return 1;
        }
        struct pacha_ipc_fd fds[PACHA_IPC_MAX_TRANSFER_FDS];
        struct pacha_ipc_msg request;
        memset(fds, 0, sizeof(fds));
        memset(&request, 0, sizeof(request));
        request.fds = fds;
        request.fd_capacity = PACHA_IPC_MAX_TRANSFER_FDS;
        const int status = pacha_ipc_recv((int)cfg->drm_endpoint_fd, &request);
        if (status == 0) {
            (void)drmd_service_dispatch(&service, &request, fds);
            (void)drmd_drm_island_reap_hangups(&island);
            active_grace_ticks = DRMD_ACTIVE_GRACE_TICKS;
        } else if (status == PACHA_ERR_EMPTY || status == PACHA_ERR_NOT_READY) {
            (void)kb_handle_any_irq(0);
            kb_run_deferred_work();
            drmd_drm_island_notify_readable(&island);
            (void)pacha_service_wait(
                &wait_set,
                active_grace_ticks != 0 ? 1u : PACHA_FD_WAIT_FOREVER);
            (void)drmd_drm_island_reap_hangups(&island);
            if (active_grace_ticks != 0) active_grace_ticks--;
        } else {
            return 1;
        }
    }
}
