#include "netd_internal.h"

#include "kobox/module.h"
#include "libuinet_backend.h"
#include "netlink_socket.h"
#include "socket_service.h"
#include "unix_socket.h"
#include "pacha/capsule.h"
#include "pacha/ipc.h"
#include "pacha/bootstrap.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int validate_boot_config(const struct netd_boot_config *cfg)
{
    if (cfg == NULL ||
        cfg->magic != NETD_BOOT_CONFIG_MAGIC ||
        cfg->version != NETD_BOOT_CONFIG_VERSION ||
        cfg->device_fd < 16 ||
        cfg->socket_endpoint_fd < 16 ||
        (cfg->flags & ~(NETD_BOOT_FLAG_SMOKE | NETD_BOOT_FLAG_TRACE | NETD_BOOT_FLAG_METRIC)) != 0) {
        if (cfg != NULL) {
            fprintf(stderr,
                "[netd] invalid boot config magic=0x%llx version=%llu fd=%llu socket_fd=%llu\n",
                (unsigned long long)cfg->magic,
                (unsigned long long)cfg->version,
                (unsigned long long)cfg->device_fd,
                (unsigned long long)cfg->socket_endpoint_fd);
        }
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    (void)argc;
    struct netd_boot_config config;
    memset(&config, 0, sizeof(config));
    const int bootstrap_fd = pacha_bootstrap_fd_from_argv(argv);
    if (bootstrap_fd < 16 ||
        pacha_fd_read(bootstrap_fd, &config, sizeof(config)) != (long)sizeof(config) ||
        !validate_boot_config(&config)) {
        return 2;
    }
    const struct netd_boot_config *cfg = &config;

    struct netd_runtime runtime;
    memset(&runtime, 0, sizeof(runtime));
    runtime.cfg = cfg;

    netd_metrics_set_enabled((cfg->flags & NETD_BOOT_FLAG_METRIC) != 0);

    printf("[netd] start device_fd=%llu socket_fd=%llu module_source=rootfs loader=%s\n",
        (unsigned long long)cfg->device_fd,
        (unsigned long long)cfg->socket_endpoint_fd,
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
    struct pacha_capsule_irq wake_irq;
    memset(&wake_irq, 0, sizeof(wake_irq));
    if (pacha_capsule_device_derive_irq(
            (int)cfg->device_fd,
            PACHA_CAPSULE_IRQ_AUTO,
            0,
            0,
            &wake_irq) != 0)
        return 8;

    printf("[netd] ready\n");
    netd_metrics_record("total_to_ready", total_start_cycles, netd_metrics_read_tsc());
    netd_metrics_print();
    fflush(stdout);
    fflush(stderr);

    for (;;) {
        uint64_t next_irq_count = 0;
        if (pacha_capsule_irq_poll(
                wake_irq.fd, wake_irq.count, &next_irq_count) == 0)
            wake_irq.count = next_irq_count;
        netd_packet_io_pump_once();
        netd_socket_service_poll();
        static struct pacha_service_wait_set wait_set;
        if (pacha_service_wait_init(&wait_set, (int)cfg->socket_endpoint_fd) != 0 ||
            pacha_service_wait_add(
                &wait_set, wake_irq.fd, PACHA_FD_EVENT_READABLE) != 0 ||
            netd_libuinet_collect_runtime_wait_sources(&wait_set) != 0 ||
            netd_socket_service_collect_wait_sources(&wait_set) != 0 ||
            netd_unix_socket_collect_wait_sources(&wait_set) != 0 ||
            netd_netlink_socket_collect_wait_sources(&wait_set) != 0 ||
            netd_libuinet_socket_collect_wait_sources(&wait_set) != 0)
            return 8;
        (void)pacha_service_wait(&wait_set, PACHA_FD_WAIT_FOREVER);
        netd_socket_service_reap_hangups(&wait_set);
        netd_unix_socket_reap_hangups(&wait_set);
        netd_netlink_socket_reap_hangups(&wait_set);
        netd_libuinet_socket_reap_hangups(&wait_set);
    }
}
