#include "netd_internal.h"

#include <stdio.h>

const char *netd_status_name(kb_status_t status)
{
    switch (status) {
    case KB_OK:
        return "KB_OK";
    case KB_ERR_INVALID:
        return "KB_ERR_INVALID";
    case KB_ERR_NOT_FOUND:
        return "KB_ERR_NOT_FOUND";
    case KB_ERR_DENIED:
        return "KB_ERR_DENIED";
    case KB_ERR_NOMEM:
        return "KB_ERR_NOMEM";
    case KB_ERR_IO:
        return "KB_ERR_IO";
    case KB_ERR_UNSUPPORTED:
        return "KB_ERR_UNSUPPORTED";
    case KB_ERR_PCI_CONFIG:
        return "KB_ERR_PCI_CONFIG";
    default:
        return "KB_ERR_UNKNOWN";
    }
}

int netd_module_stack_load(struct netd_runtime *runtime)
{
    if (runtime == NULL || runtime->cfg == NULL || runtime->backend == NULL) {
        return 4;
    }

    for (uint64_t i = 0; i < runtime->cfg->module_count; i++) {
        const struct netd_module_config *module_cfg = &runtime->cfg->modules[i];
        if (module_cfg->image_va == 0 || module_cfg->image_size == 0 || module_cfg->name[0] == '\0') {
            fprintf(stderr, "[netd] invalid module slot=%llu\n", (unsigned long long)i);
            return 4;
        }
        const kb_module_image_t image = {
            .data = (const void *)(uintptr_t)module_cfg->image_va,
            .size = (size_t)module_cfg->image_size,
            .name = module_cfg->name,
        };
        uint64_t stage_start_cycles = netd_metrics_read_tsc();
        kb_status_t status = kb_module_open_image(&image, runtime->backend, &runtime->modules[i]);
        uint64_t stage_end_cycles = netd_metrics_read_tsc();
        netd_metrics_record_ex(
            "module_open",
            module_cfg->name,
            stage_start_cycles,
            stage_end_cycles,
            module_cfg->image_size);
        if (status != KB_OK || runtime->modules[i] == NULL) {
            fprintf(stderr, "[netd] %s open failed status=%s(%d)\n",
                module_cfg->name,
                netd_status_name(status),
                status);
            return 4;
        }
    }
    return 0;
}
