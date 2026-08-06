#include "netd_internal.h"
#include "filed_client/module_image.h"

#include <stdio.h>

const char *const netd_module_names[] = {
    "virtio.ko", "virtio_ring.ko", "virtio_pci.ko",
    "failover.ko", "net_failover.ko", "virtio_net.ko",
};

static const char *const module_paths[] = {
    "/usr/lib/kobox/virtio.ko", "/usr/lib/kobox/virtio_ring.ko",
    "/usr/lib/kobox/virtio_pci.ko", "/usr/lib/kobox/failover.ko",
    "/usr/lib/kobox/net_failover.ko", "/usr/lib/kobox/virtio_net.ko",
};

const uint64_t netd_module_count = sizeof(netd_module_names) / sizeof(netd_module_names[0]);

_Static_assert(sizeof(module_paths) == sizeof(netd_module_names),
    "netd module name and path tables must stay in sync");
_Static_assert(sizeof(netd_module_names) / sizeof(netd_module_names[0]) <= NETD_MAX_MODULES,
    "netd module table exceeds runtime capacity");

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

    runtime->module_count = netd_module_count;
    for (uint64_t i = 0; i < runtime->module_count; i++) {
        struct filed_client_module_image loaded;
        const int load_status = filed_client_load_module_image(
            FILED_CLIENT_ENDPOINT_FD, module_paths[i], netd_module_names[i], &loaded);
        if (load_status != 0) {
            fprintf(stderr, "[netd] module read failed path=%s status=%d\n",
                module_paths[i], load_status);
            return 4;
        }
        const kb_module_image_t image = {
            .data = loaded.data,
            .size = loaded.size,
            .name = loaded.name,
        };
        uint64_t stage_start_cycles = netd_metrics_read_tsc();
        kb_status_t status = kb_module_open_image(&image, runtime->backend, &runtime->modules[i]);
        uint64_t stage_end_cycles = netd_metrics_read_tsc();
        netd_metrics_record_ex(
            "module_open",
            loaded.name,
            stage_start_cycles,
            stage_end_cycles,
            loaded.size);
        if (status != KB_OK || runtime->modules[i] == NULL) {
            fprintf(stderr, "[netd] %s open failed status=%s(%d)\n",
                loaded.name,
                netd_status_name(status),
                status);
            return 4;
        }
    }
    return 0;
}
