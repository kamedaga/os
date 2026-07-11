#include "drmd/boot_config.h"
#include "drmd_service.h"

#include <pacha/abi.h>
#include <pacha/ipc.h>
#include <kobox/shim.h>

#include <stdio.h>
#include <string.h>

int main(void)
{
    const struct drmd_boot_config *cfg =
        (const struct drmd_boot_config *)(uintptr_t)DRMD_BOOT_CONFIG_VA;
    if (cfg == NULL || cfg->magic != DRMD_BOOT_CONFIG_MAGIC ||
        cfg->drm_endpoint_fd < 16 || cfg->device_fd < 16 ||
        cfg->ready_channel_fd < 16) {
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
    for (;;) {
        struct pacha_ipc_fd fds[PACHA_IPC_MAX_TRANSFER_FDS];
        struct pacha_ipc_msg request;
        memset(fds, 0, sizeof(fds));
        memset(&request, 0, sizeof(request));
        request.fds = fds;
        request.fd_capacity = PACHA_IPC_MAX_TRANSFER_FDS;
        const int status = pacha_ipc_recv((int)cfg->drm_endpoint_fd, &request);
        if (status == 0) {
            (void)drmd_service_dispatch(&service, &request, fds);
        } else if (status != PACHA_ERR_EMPTY && status != PACHA_ERR_NOT_READY) {
            return 1;
        }
        kb_run_deferred_work();
        if (kb_handle_any_irq(0) == 0) {
            drmd_drm_island_handle_irq(&island);
        }
    }
}
