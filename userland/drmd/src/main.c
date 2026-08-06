#include "drmd/boot_config.h"
#include "drm_kms.h"
#include "drmd_service.h"

#include <pacha/abi.h>
#include <pacha/capsule.h>
#include <pacha/ipc.h>
#include <pacha/bootstrap.h>
#include <kobox/shim.h>

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    (void)argc;
    struct drmd_boot_config config;
    memset(&config, 0, sizeof(config));
    const int bootstrap_fd = pacha_bootstrap_fd_from_argv(argv);
    const struct drmd_boot_config *cfg = &config;
    if (bootstrap_fd < 16 ||
        pacha_fd_read(bootstrap_fd, &config, sizeof(config)) != (long)sizeof(config) ||
        cfg->magic != DRMD_BOOT_CONFIG_MAGIC ||
        cfg->version != DRMD_BOOT_CONFIG_VERSION ||
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
    struct pacha_capsule_irq wake_irq;
    memset(&wake_irq, 0, sizeof(wake_irq));
    if (pacha_capsule_device_derive_irq(
            (int)cfg->device_fd, PACHA_CAPSULE_IRQ_AUTO, 0, 0, &wake_irq) != 0)
        return 1;
    for (;;) {
        static struct pacha_service_wait_set wait_set;
        if (pacha_service_wait_init(&wait_set, (int)cfg->drm_endpoint_fd) != 0)
            return 1;
        if (pacha_service_wait_add(
                &wait_set, wake_irq.fd, PACHA_FD_EVENT_READABLE) != 0)
            return 1;
        int notify_fds[DRMD_DRM_WAIT_SOURCE_MAX];
        const size_t notify_count = drmd_drm_island_collect_wait_sources(
            notify_fds, DRMD_DRM_WAIT_SOURCE_MAX);
        for (size_t i = 0; i < notify_count; i++) {
            if (pacha_service_wait_add(
                    &wait_set, notify_fds[i], PACHA_FD_EVENT_HANGUP) != 0)
                return 1;
        }
        int kms_wait_fds[DRMD_DRM_WAIT_SOURCE_MAX];
        size_t kms_wait_capacity = PACHA_SERVICE_WAIT_MAX_FDS > wait_set.count ?
            PACHA_SERVICE_WAIT_MAX_FDS - (size_t)wait_set.count : 0;
        if (kms_wait_capacity > DRMD_DRM_WAIT_SOURCE_MAX) {
            kms_wait_capacity = DRMD_DRM_WAIT_SOURCE_MAX;
        }
        const size_t kms_wait_count = drmd_kms_collect_wait_sources(
            kms_wait_fds, kms_wait_capacity);
        for (size_t i = 0; i < kms_wait_count; i++) {
            if (pacha_service_wait_add(
                    &wait_set,
                    kms_wait_fds[i],
                    PACHA_FD_EVENT_READABLE |
                        PACHA_FD_EVENT_ERROR | PACHA_FD_EVENT_HANGUP) != 0) {
                return 1;
            }
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
        } else if (status != PACHA_ERR_EMPTY && status != PACHA_ERR_NOT_READY) {
            return 1;
        }
        uint64_t next_count = 0;
        if (pacha_capsule_irq_poll(wake_irq.fd, wake_irq.count, &next_count) == 0)
            wake_irq.count = next_count;
        (void)kb_handle_any_irq(0);
        kb_run_deferred_work();
        drmd_kms_progress_page_flip();
        (void)drmd_service_progress(&service);
        drmd_drm_island_notify_readable(&island);
        (void)drmd_drm_island_reap_hangups(&island);
        if (status == PACHA_ERR_EMPTY || status == PACHA_ERR_NOT_READY) {
            (void)pacha_service_wait(&wait_set, PACHA_FD_WAIT_FOREVER);
            (void)drmd_drm_island_reap_hangups(&island);
        }
    }
}
