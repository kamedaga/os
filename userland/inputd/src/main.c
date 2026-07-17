#include <inputd/boot_config.h>
#include "inputd_service.h"

#include <pacha/abi.h>
#include <pacha/capsule.h>
#include <pacha/ipc.h>
#include <kobox/shim.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int inputd_boot_config_validate(const struct inputd_boot_config *cfg)
{
    if (cfg == NULL || cfg->magic != INPUTD_BOOT_CONFIG_MAGIC ||
        cfg->version != INPUTD_BOOT_CONFIG_VERSION ||
        cfg->header_size != sizeof(*cfg) ||
        cfg->total_size > INPUTD_BOOT_CONFIG_MAX_BYTES ||
        cfg->input_endpoint_fd < 16 || cfg->ready_channel_fd < 16 ||
        cfg->netd_endpoint_fd < 16 || cfg->device_count == 0 ||
        cfg->module_count == 0 ||
        cfg->device_record_size != sizeof(struct inputd_device_config) ||
        cfg->module_record_size != sizeof(struct inputd_module_config) ||
        cfg->devices_offset != sizeof(*cfg)) return -22;
    const uint64_t device_capacity =
        (INPUTD_BOOT_CONFIG_MAX_BYTES - sizeof(*cfg)) / sizeof(struct inputd_device_config);
    if (cfg->device_count > device_capacity) return -22;
    const uint64_t modules_offset = sizeof(*cfg) +
        (uint64_t)cfg->device_count * sizeof(struct inputd_device_config);
    if (cfg->modules_offset != modules_offset ||
        modules_offset > INPUTD_BOOT_CONFIG_MAX_BYTES) return -22;
    const uint64_t module_capacity =
        (INPUTD_BOOT_CONFIG_MAX_BYTES - modules_offset) / sizeof(struct inputd_module_config);
    if (cfg->module_count > module_capacity ||
        cfg->total_size != modules_offset +
            (uint64_t)cfg->module_count * sizeof(struct inputd_module_config) ||
        cfg->device_count >= PACHA_SERVICE_WAIT_MAX_FDS - 1u) return -22;

    const struct inputd_device_config *devices = inputd_boot_devices(cfg);
    for (uint32_t i = 0; i < cfg->device_count; i++) {
        if (devices[i].device_fd < 16 || devices[i].device_fd >= 256 ||
            devices[i].pci_segment > UINT16_MAX || devices[i].pci_bus > UINT8_MAX ||
            devices[i].pci_device > 31 || devices[i].pci_function > 7 ||
            devices[i].vendor_id > UINT16_MAX || devices[i].device_id > UINT16_MAX ||
            devices[i].subsystem_id > UINT16_MAX) return -22;
    }
    const struct inputd_module_config *modules = inputd_boot_modules(cfg);
    for (uint32_t i = 0; i < cfg->module_count; i++) {
        if (modules[i].image_va == 0 || modules[i].image_size == 0 ||
            memchr(modules[i].name, '\0', sizeof(modules[i].name)) == NULL) return -22;
    }
    return 0;
}

int main(void)
{
    const struct inputd_boot_config *cfg =
        (const struct inputd_boot_config *)(uintptr_t)INPUTD_BOOT_CONFIG_VA;
    if (inputd_boot_config_validate(cfg) != 0) return 1;
    static struct inputd_input_island island;
    inputd_service_t service = { .cfg = cfg, .input = &island };
    int init_status = inputd_input_island_init(&island, cfg);
    struct pacha_capsule_irq *wake_irqs = NULL;
    const struct inputd_device_config *devices = inputd_boot_devices(cfg);
    if (init_status == 0) {
        wake_irqs = calloc(cfg->device_count, sizeof(*wake_irqs));
        if (wake_irqs == NULL) init_status = -12;
    }
    for (size_t i = 0; init_status == 0 && i < cfg->device_count; i++) {
        if (pacha_capsule_device_derive_irq(
                (int)devices[i].device_fd, PACHA_CAPSULE_IRQ_AUTO, 0, 0,
                &wake_irqs[i]) != 0)
            init_status = -5;
    }
    if (init_status == 0) init_status = inputd_service_publish_startup_devices(&service);
    printf("[inputd] init status=%d modules=%u devices=%u ready=%d\n",
        init_status, island.loaded_module_count, island.device_count, island.ready);
    const int ready_status = inputd_service_send_boot_ready(
        &service, init_status, island.ready ? island.device_count : 0);
    if (ready_status != 0 || init_status != 0) return 1;
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
        for (size_t i = 0; i < cfg->device_count; i++) {
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
            for (size_t i = 0; i < cfg->device_count; i++) {
                if (pacha_service_wait_add(
                        &wait_set, wake_irqs[i].fd, PACHA_FD_EVENT_READABLE) != 0)
                    return 1;
            }
            const size_t handle_wait_count = inputd_input_collect_wait_sources(
                handle_wait_fds,
                PACHA_SERVICE_WAIT_MAX_FDS - 1u - cfg->device_count);
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
