#include <inputd/boot_config.h>
#include "inputd_service.h"

#include <pacha/abi.h>
#include <pacha/capsule.h>
#include <pacha/ipc.h>
#include <kobox/shim.h>

#include <stdio.h>
#include <string.h>

int main(void)
{
    const struct inputd_boot_config *cfg =
        (const struct inputd_boot_config *)(uintptr_t)INPUTD_BOOT_CONFIG_VA;
    if (cfg == NULL || cfg->magic != INPUTD_BOOT_CONFIG_MAGIC ||
        cfg->input_endpoint_fd < 16 || cfg->ready_channel_fd < 16 ||
        cfg->netd_endpoint_fd < 16 ||
        cfg->device_count != INPUTD_DEVICE_COUNT) return 1;
    static struct inputd_input_island island;
    inputd_service_t service = { .cfg = cfg, .input = &island };
    const int init_status = inputd_input_island_init(&island, cfg);
    printf("[inputd] init status=%d modules=%u devices=%u ready=%d\n",
        init_status, island.loaded_module_count, island.device_count, island.ready);
    const int ready_status = inputd_service_send_boot_ready(
        &service, init_status, island.ready ? island.device_count : 0);
    if (ready_status != 0 || init_status != 0) return 1;
    struct pacha_capsule_irq wake_irqs[INPUTD_DEVICE_COUNT];
    memset(wake_irqs, 0, sizeof(wake_irqs));
    for (size_t i = 0; i < INPUTD_DEVICE_COUNT; i++) {
        if (pacha_capsule_device_derive_irq(
                (int)cfg->device_fds[i], PACHA_CAPSULE_IRQ_AUTO, 0, 0, &wake_irqs[i]) != 0)
            return 1;
    }
    printf("[inputd] service loop endpoint_fd=%llu\n",
        (unsigned long long)cfg->input_endpoint_fd);
    for (;;) {
        struct pacha_ipc_fd fds[PACHA_IPC_MAX_TRANSFER_FDS];
        struct pacha_ipc_msg request;
        memset(fds, 0, sizeof(fds));
        memset(&request, 0, sizeof(request));
        request.fds = fds;
        request.fd_capacity = PACHA_IPC_MAX_TRANSFER_FDS;
        const int status = pacha_ipc_recv((int)cfg->input_endpoint_fd, &request);
        if (status == 0) {
            (void)inputd_service_dispatch(&service, &request, fds);
        } else if (status != PACHA_ERR_EMPTY && status != PACHA_ERR_NOT_READY) {
            fprintf(stderr, "[inputd] service receive failed status=%d endpoint_fd=%llu\n",
                status, (unsigned long long)cfg->input_endpoint_fd);
            return 1;
        }
        for (size_t i = 0; i < INPUTD_DEVICE_COUNT; i++) {
            uint64_t next_count = 0;
            if (pacha_capsule_irq_poll(
                    wake_irqs[i].fd, wake_irqs[i].count, &next_count) == 0)
                wake_irqs[i].count = next_count;
        }
        inputd_input_island_pump(&island);
        inputd_input_notify_readable();
        (void)inputd_input_reap_hangups();
        if (status == PACHA_ERR_EMPTY || status == PACHA_ERR_NOT_READY) {
            static struct pacha_service_wait_set wait_set;
            int handle_wait_fds[PACHA_SERVICE_WAIT_MAX_FDS];
            if (pacha_service_wait_init(&wait_set, (int)cfg->input_endpoint_fd) != 0)
                return 1;
            for (size_t i = 0; i < INPUTD_DEVICE_COUNT; i++) {
                if (pacha_service_wait_add(
                        &wait_set, wake_irqs[i].fd, PACHA_FD_EVENT_READABLE) != 0)
                    return 1;
            }
            const size_t handle_wait_count = inputd_input_collect_wait_sources(
                handle_wait_fds,
                PACHA_SERVICE_WAIT_MAX_FDS - 1u - INPUTD_DEVICE_COUNT);
            for (size_t i = 0; i < handle_wait_count; i++) {
                if (pacha_service_wait_add(
                        &wait_set, handle_wait_fds[i], PACHA_FD_EVENT_HANGUP) != 0)
                    return 1;
            }
            (void)pacha_service_wait(&wait_set, PACHA_FD_WAIT_FOREVER);
            (void)inputd_input_reap_hangups();
        }
    }
}
