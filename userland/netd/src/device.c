#define _GNU_SOURCE

#include "netd_internal.h"

#include "kobox/device_pachaos_capsule.h"
#include "kobox/shim.h"
#include "linux_subsystem/kvm/kvm_symbols.h"
#include "pacha/capsule.h"

#include <stdio.h>
#include <stdlib.h>

static void configure_netd_env(uint64_t flags)
{
    (void)setenv("KOBOX_DEVICE_BACKEND", "pachaos", 1);
    (void)setenv("KOBOX_NET_DRIVER", "virtio", 1);
    (void)setenv("KOBOX_NET_AUTO_OPEN", "1", 1);
    (void)setenv("KOBOX_VIRTIO_NO_INDIRECT", "1", 1);
    (void)setenv("KOBOX_VIRTIO_NO_EVENT_IDX", "1", 1);
    if ((flags & NETD_BOOT_FLAG_METRIC) != 0) {
        (void)setenv("KOBOX_NET_METRIC", "1", 1);
    }
    if ((flags & NETD_BOOT_FLAG_SMOKE) != 0) {
        (void)setenv("KOBOX_NET_RX_POLL_SMOKE", "1", 1);
    }
    if ((flags & NETD_BOOT_FLAG_TRACE) != 0) {
        (void)setenv("KOBOX_TRACE_NET", "1", 1);
        (void)setenv("KOBOX_TRACE_VIRTIO", "1", 1);
        (void)setenv("KOBOX_TRACE_PCI", "1", 1);
        (void)setenv("KOBOX_TRACE_DMA", "1", 1);
        (void)setenv("KOBOX_TRACE_KVM", "1", 1);
        (void)setenv("KOBOX_TRACE_KVM_RELOC", "1", 1);
    }
}

static void log_pci_command(uint64_t device_fd, const char *label)
{
    uint32_t command = 0;
    int status = pacha_capsule_pci_config_read((int)device_fd, 0x04, 2, &command);
    printf("[netd] pci command %s status=%d value=0x%04x\n",
        label,
        status,
        command & 0xffffu);
}

static void log_pci_probe(uint64_t device_fd)
{
    for (unsigned bar_index = 0; bar_index < 2; bar_index++) {
        struct pacha_capsule_bar_info bar = {0};
        int bar_status = pacha_capsule_pci_bar_info((int)device_fd, bar_index, &bar);
        printf("[netd] pci bar%u status=%d start=0x%llx end=0x%llx size=0x%llx flags=0x%llx\n",
            bar_index,
            bar_status,
            (unsigned long long)bar.start,
            (unsigned long long)bar.end,
            (unsigned long long)bar.size,
            (unsigned long long)bar.flags);
    }
    for (uint16_t offset = 0x10; offset <= 0x14; offset += 4) {
        uint32_t value = 0;
        int config_status = pacha_capsule_pci_config_read((int)device_fd, offset, 4, &value);
        printf("[netd] pci config[0x%02x] status=%d value=0x%08x\n",
            offset,
            config_status,
            value);
    }
}

int netd_device_init(struct netd_runtime *runtime)
{
    if (runtime == NULL || runtime->cfg == NULL) {
        return 3;
    }

    uint64_t stage_start_cycles = netd_metrics_read_tsc();
    kb_status_t status = kb_pachaos_capsule_device_create(runtime->cfg->device_fd, &runtime->backend);
    netd_metrics_record("device_create", stage_start_cycles, netd_metrics_read_tsc());
    if (status != KB_OK || runtime->backend == NULL) {
        fprintf(stderr, "[netd] device backend create failed status=%d\n", status);
        return 3;
    }

    stage_start_cycles = netd_metrics_read_tsc();
    kb_shim_set_device_backend(runtime->backend);
    configure_netd_env(runtime->cfg->flags);
    netd_metrics_record("shim_env", stage_start_cycles, netd_metrics_read_tsc());

    stage_start_cycles = netd_metrics_read_tsc();
    int arena_status = kb_kvm_prepare_dma_arena(runtime->backend);
    netd_metrics_record("dma_arena", stage_start_cycles, netd_metrics_read_tsc());
    if (arena_status != 0) {
        fprintf(stderr, "[netd] dma arena prepare failed status=%d\n", arena_status);
        return 3;
    }

    if ((runtime->cfg->flags & NETD_BOOT_FLAG_TRACE) != 0) {
        stage_start_cycles = netd_metrics_read_tsc();
        log_pci_probe(runtime->cfg->device_fd);
        log_pci_command(runtime->cfg->device_fd, "before-init");
        netd_metrics_record("pci_probe_log", stage_start_cycles, netd_metrics_read_tsc());
    }
    return 0;
}
