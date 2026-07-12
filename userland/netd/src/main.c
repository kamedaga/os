#include "netd_internal.h"

#include "kobox/module.h"
#include "libuinet_backend.h"
#include "socket_service.h"
#include "pacha/ipc.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int validate_boot_config(const struct netd_boot_config *cfg)
{
    if (cfg == NULL ||
        cfg->magic != NETD_BOOT_CONFIG_MAGIC ||
        cfg->device_fd < 16 ||
        cfg->socket_endpoint_fd < 16 ||
        cfg->module_count == 0 ||
        cfg->module_count > NETD_MAX_MODULES ||
        (cfg->flags & ~(NETD_BOOT_FLAG_SMOKE | NETD_BOOT_FLAG_TRACE | NETD_BOOT_FLAG_METRIC)) != 0) {
        if (cfg != NULL) {
            fprintf(stderr,
                "[netd] invalid boot config magic=0x%llx fd=%llu socket_fd=%llu modules=%llu\n",
                (unsigned long long)cfg->magic,
                (unsigned long long)cfg->device_fd,
                (unsigned long long)cfg->socket_endpoint_fd,
                (unsigned long long)cfg->module_count);
        }
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    const struct netd_boot_config *cfg =
        (const struct netd_boot_config *)(uintptr_t)NETD_BOOT_CONFIG_VA;
    if (!validate_boot_config(cfg)) {
        return 2;
    }

    struct netd_runtime runtime;
    memset(&runtime, 0, sizeof(runtime));
    runtime.cfg = cfg;

    netd_metrics_set_enabled((cfg->flags & NETD_BOOT_FLAG_METRIC) != 0);

    printf("[netd] start device_fd=%llu socket_fd=%llu modules=%llu loader=%s\n",
        (unsigned long long)cfg->device_fd,
        (unsigned long long)cfg->socket_endpoint_fd,
        (unsigned long long)cfg->module_count,
        kb_module_loader_version());
    uint64_t total_start_cycles = netd_metrics_read_tsc();

    int status = netd_device_init(&runtime);
    if (status != 0) {
        return status;
    }
    status = netd_module_stack_load(&runtime);
    if (status != 0) {
        return status;
    }
    status = netd_driver_bind(&runtime);
    if (status != 0) {
        return status;
    }
    status = netd_packet_io_start(&runtime);
    if (status != 0) {
        return status;
    }
    status = netd_socket_service_start(&runtime);
    if (status != 0) {
        return status;
    }

    printf("[netd] ready\n");
    netd_metrics_record("total_to_ready", total_start_cycles, netd_metrics_read_tsc());
    netd_metrics_print();
    fflush(stdout);
    fflush(stderr);

    static struct pacha_service_wait_set wait_set;
    if (pacha_service_wait_init(&wait_set, (int)cfg->socket_endpoint_fd) != 0) return 8;

    for (;;) {
        netd_packet_io_pump_once();
        netd_socket_service_poll();
        (void)pacha_service_wait(
            &wait_set,
            netd_libuinet_needs_periodic_poll() ? 1u : PACHA_FD_WAIT_FOREVER);
    }
}
