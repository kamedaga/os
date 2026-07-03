#include "netd_internal.h"

#include "pacha/capsule.h"

#include <stdio.h>

static void log_pci_command(uint64_t device_fd, const char *label)
{
    uint32_t command = 0;
    int status = pacha_capsule_pci_config_read((int)device_fd, 0x04, 2, &command);
    printf("[netd] pci command %s status=%d value=0x%04x\n",
        label,
        status,
        command & 0xffffu);
}

int netd_driver_bind(struct netd_runtime *runtime)
{
    if (runtime == NULL || runtime->cfg == NULL) {
        return 5;
    }

    for (uint64_t i = 0; i < runtime->cfg->module_count; i++) {
        const struct netd_module_config *module_cfg = &runtime->cfg->modules[i];
        int init_result = 0;
        printf("[netd] %s init begin\n", module_cfg->name);
        fflush(stdout);
        uint64_t stage_start_cycles = netd_metrics_read_tsc();
        kb_status_t status = kb_module_call_init(runtime->modules[i], &init_result);
        uint64_t stage_end_cycles = netd_metrics_read_tsc();
        netd_metrics_record_ex("module_init", module_cfg->name, stage_start_cycles, stage_end_cycles, 0);
        if (status == KB_ERR_NOT_FOUND && i + 1u < runtime->cfg->module_count) {
            printf("[netd] %s has no init_module\n", module_cfg->name);
            continue;
        }
        printf("[netd] %s init returned status=%s(%d) result=%d\n",
            module_cfg->name,
            netd_status_name(status),
            status,
            init_result);
        fflush(stdout);
        if (status != KB_OK || init_result != 0) {
            fprintf(stderr,
                "[netd] %s init failed status=%s(%d) result=%d\n",
                module_cfg->name,
                netd_status_name(status),
                status,
                init_result);
            return 5;
        }
    }

    if ((runtime->cfg->flags & NETD_BOOT_FLAG_TRACE) != 0) {
        uint64_t stage_start_cycles = netd_metrics_read_tsc();
        log_pci_command(runtime->cfg->device_fd, "after-init");
        netd_metrics_record("pci_after_log", stage_start_cycles, netd_metrics_read_tsc());
    }
    return 0;
}
