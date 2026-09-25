#define _GNU_SOURCE

#include "netd_internal.h"

#include "libuinet_backend.h"
#include "netlink_socket.h"
#include "network_config.h"
#include "status_file.h"
#include "link.h"
#include "kobox2_nic.h"
#include "socket_service.h"
#include "upper.h"
#include "pacha/capsule.h"
#include "pacha/ipc.h"
#include "pacha/bootstrap.h"

#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int validate_boot_config(const struct netd_boot_config *cfg)
{
    if (cfg == NULL ||
        cfg->magic != NETD_BOOT_CONFIG_MAGIC ||
        cfg->version != NETD_BOOT_CONFIG_VERSION ||
        cfg->device_fd < 16 ||
        cfg->filed_endpoint_fd < 16 ||
        cfg->socket_endpoint_fd < 16 ||
        (cfg->status_channel_fd != 0 && cfg->status_channel_fd < 16) ||
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

static int report_link_status(const struct netd_boot_config *cfg,
    int status, uint64_t stage)
{
    netd_status_file_publish(cfg, status ? "failed" : "ready", stage, status);
    if (cfg->status_channel_fd < 16) return 0;
    const struct netd_link_info *link = status == 0 ? netd_link_current() : NULL;
    uint64_t metadata = stage;
    uint64_t detail = link ? ((uint64_t)link->mtu << 8) | link->carrier : 0;
    uint64_t result = (uint32_t)status;
    if (status && stage == NETD_BOOT_STAGE_NIC) {
        const struct netd_nic_diagnostic *diagnostic =
            netd_kobox2_nic_diagnostic();
        if (diagnostic->fault) {
            /* A fault needs both the instruction and accessed address. Use
             * its own status layout so neither is truncated to a log hint. */
            metadata = diagnostic->fault_ip;
            result |= (uint64_t)netd_boot_fault_metadata(
                diagnostic->fault_vector, diagnostic->fault_error_code,
                diagnostic->fault_core_relative) << 32;
            detail = diagnostic->fault_address;
        } else {
            metadata |= ((uint64_t)(diagnostic->step & 0xffu) << 8) |
                ((uint64_t)(diagnostic->loaded & 0xffu) << 16) |
                ((uint64_t)(diagnostic->pci_bound & 1u) << 24) |
                ((uint64_t)diagnostic->line << 32);
            result |= (uint64_t)(uint32_t)diagnostic->detail << 32;
            detail = diagnostic->source;
        }
    }
    const struct pacha_ipc_msg report = {
        .word0 = status && stage == NETD_BOOT_STAGE_NIC &&
            netd_kobox2_nic_diagnostic()->fault ?
            NETD_BOOT_FAULT_MAGIC : NETD_BOOT_STATUS_MAGIC,
        .word1 = result,
        .word2 = metadata,
        .word3 = detail,
    };
    return pacha_ipc_send((int)cfg->status_channel_fd, &report);
}

static int netd_wait_set_failure(
    const char *collector,
    int status,
    const struct pacha_service_wait_set *wait_set)
{
    fprintf(stderr,
        "[netd] wait_set_collect_failed collector=%s status=%d count=%llu capacity=%u\n",
        collector,
        status,
        (unsigned long long)(wait_set != NULL ? wait_set->count : 0),
        (unsigned)PACHA_SERVICE_WAIT_MAX_FDS);
    fflush(stderr);
    return 8;
}

/* A live RAM root starts without site policy. Looking for this ordinary file
 * after NIC startup lets ash supply policy later, without baking a test LAN
 * address into the reusable image or keeping the driver offline meanwhile. */
static int netd_try_ipv4_policy(struct netd_runtime *runtime, int *state)
{
    static int last_policy_error;
    if (!runtime || !runtime->cfg || !state || *state != 0) return 0;
    const struct netd_boot_config *cfg = runtime->cfg;
    struct netd_ipv4_boot_config ipv4 = cfg->ipv4;
    if (!ipv4.address[0] && !ipv4.address[1] &&
        !ipv4.address[2] && !ipv4.address[3]) {
        char text[512];
        size_t text_size = 0;
        int error = netd_policy_file_read((int)cfg->filed_endpoint_fd,
            text, sizeof(text), &text_size);
        if (error) {
            if (error != -ENOENT && error != last_policy_error)
                fprintf(stderr, "[netd] IPv4 policy read failed status=%d; link remains active\n", error);
            last_policy_error = error;
            return 0;
        }
        if (netd_network_config_parse_text(text, &ipv4) != 0) {
            if (last_policy_error != -EINVAL) {
                fprintf(stderr,
                    "[netd] IPv4 policy malformed size=%zu text=\"%s\"; link remains active\n",
                    text_size, text);
                netd_status_file_note(cfg,
                    "IPv4 policy parse failed; inspect /run/pacha/network.conf");
            }
            last_policy_error = -EINVAL;
            return 0;
        }
    }
    if (netd_network_config_init(&ipv4) != 0) {
        if (last_policy_error != -EINVAL)
            fprintf(stderr, "[netd] IPv4 policy invalid; link remains active\n");
        last_policy_error = -EINVAL;
        return 0;
    }
    last_policy_error = 0;
    int status = netd_upper_start(runtime);
    if (!status && (cfg->flags & NETD_BOOT_FLAG_SMOKE) != 0)
        status = netd_packet_io_smoke_after_ipv4();
    if (status != 0) {
        fprintf(stderr, "[netd] IPv4 stack failed status=%d; NIC remains active\n", status);
        *state = -1;
        return status;
    }
    *state = 1;
    printf("[netd] IPv4 policy applied after NIC startup\n");
    /* NIC readiness precedes DHCP and libuinet startup. Publish a separate
     * readiness event so dependent services cannot race the TCP/IP stack. */
    (void)report_link_status(cfg, 0, NETD_BOOT_STAGE_IPV4);
    int resolver_status = netd_resolver_file_create(
        (int)cfg->filed_endpoint_fd, ipv4.dns);
    if (resolver_status == 0) {
        netd_status_file_note(cfg,
            "DNS resolver configured from live network policy");
    } else if (resolver_status == -ENODATA) {
        netd_status_file_note(cfg,
            "IPv4 ready; network policy has no DNS server");
    } else if (resolver_status != -EEXIST) {
        char dns_phase[80];
        (void)snprintf(dns_phase, sizeof(dns_phase),
            "DNS resolver configuration failed status=%d", resolver_status);
        netd_status_file_note(cfg, dns_phase);
    }
    char phase[80];
    (void)snprintf(phase, sizeof(phase),
        "IPv4 ready address=%u.%u.%u.%u gateway=%u.%u.%u.%u",
        ipv4.address[0], ipv4.address[1], ipv4.address[2], ipv4.address[3],
        ipv4.gateway[0], ipv4.gateway[1], ipv4.gateway[2], ipv4.gateway[3]);
    netd_status_file_note(cfg, phase);
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)setenv("KOBOX_DAEMON_NAME", "netd", 1);
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

    printf("[netd] start device_fd=%llu socket_fd=%llu module_source=kobox2\n",
        (unsigned long long)cfg->device_fd,
        (unsigned long long)cfg->socket_endpoint_fd);
    netd_status_file_publish(cfg, "initializing", NETD_BOOT_STAGE_NIC, 0);
    uint64_t total_start_cycles = netd_metrics_read_tsc();

    int status = netd_packet_io_start(&runtime);
    if (status != 0) {
        (void)report_link_status(cfg, status, NETD_BOOT_STAGE_NIC);
        return status;
    }
    netd_status_file_publish(cfg, "initializing", NETD_BOOT_STAGE_SOCKET, 0);
    status = netd_socket_service_start(&runtime);
    if (status != 0) {
        (void)report_link_status(cfg, status, NETD_BOOT_STAGE_SOCKET);
        return status;
    }
    int ipv4_state = 0;
    (void)netd_try_ipv4_policy(&runtime, &ipv4_state);
    int dhcp_owned = 0;
    if (ipv4_state == 0) {
        status = netd_packet_io_dhcp_start(&runtime);
        if (status != 0) {
            char phase[80];
            (void)snprintf(phase, sizeof(phase),
                "DHCP startup failed status=%d; NIC remains active", status);
            netd_status_file_note(cfg, phase);
        } else {
            netd_status_file_note(cfg,
                "NIC active; acquiring LAN policy via DHCP");
        }
    }
    netd_status_file_publish(cfg, "initializing", NETD_BOOT_STAGE_TIMER, 0);
    /* The Linux sandbox owns NIC IRQs. This timer bounds RX and carrier
     * latency without making libuinet or the controller multi-threaded. */
    const uint64_t timer_rights = PACHA_FD_RIGHT_READ | PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_CLOSE;
    int timer_fd = pacha_timerfd_create(UINT64_C(10000000),
        UINT64_C(10000000), timer_rights, 0);
    if (timer_fd < 16) {
        (void)report_link_status(cfg, timer_fd < 0 ? timer_fd : -5,
            NETD_BOOT_STAGE_TIMER);
        return 8;
    }

    printf("[netd] ready\n");
    netd_metrics_record("total_to_ready", total_start_cycles, netd_metrics_read_tsc());
    netd_metrics_print();
    fflush(stdout);
    fflush(stderr);
    (void)report_link_status(cfg, 0, NETD_BOOT_STAGE_READY);

    uint64_t policy_ticks = 0;
    unsigned policy_attempted_ack = 0;
    int last_reported_carrier = netd_link_current() ?
        (int)netd_link_current()->carrier : -1;
    for (;;) {
        struct pacha_pollfd timer_event = {.fd = timer_fd,
            .events = PACHA_FD_EVENT_READABLE};
        if (pacha_fd_poll(&timer_event, 1) > 0 &&
            (timer_event.revents & PACHA_FD_EVENT_READABLE)) {
            uint64_t expirations = 0;
            (void)pacha_fd_read(timer_fd, &expirations, sizeof(expirations));
            if (ipv4_state == 0) {
                policy_ticks += expirations;
                if (policy_ticks >= 100) {
                    policy_ticks %= 100;
                    (void)netd_try_ipv4_policy(&runtime, &ipv4_state);
                    if (ipv4_state == 1 && !dhcp_owned)
                        netd_packet_io_dhcp_disable();
                }
            }
        }
        netd_packet_io_pump_once();
        const struct netd_dhcp_client *dhcp = netd_packet_io_dhcp_status();
        if (ipv4_state == 0 && dhcp != NULL &&
            dhcp->state == NETD_DHCP_BOUND &&
            policy_attempted_ack != dhcp->acks) {
            policy_attempted_ack = dhcp->acks;
            int policy_status = netd_policy_file_create(
                (int)cfg->filed_endpoint_fd, &dhcp->lease);
            if (policy_status == 0) {
                dhcp_owned = 1;
                netd_status_file_note(cfg,
                    "DHCP lease committed to /run/pacha/network.conf");
            } else if (policy_status != -EEXIST) {
                char phase[80];
                (void)snprintf(phase, sizeof(phase),
                    "DHCP policy write failed status=%d", policy_status);
                netd_status_file_note(cfg, phase);
            }
            (void)netd_try_ipv4_policy(&runtime, &ipv4_state);
            if (ipv4_state == 1 && !dhcp_owned)
                netd_packet_io_dhcp_disable();
        }
        const struct netd_link_info *link = netd_link_current();
        if (link && (int)link->carrier != last_reported_carrier &&
            report_link_status(cfg, 0, NETD_BOOT_STAGE_LINK) == 0)
            last_reported_carrier = link->carrier;
        const int busy = netd_socket_service_poll();
        static struct pacha_service_wait_set wait_set;
        status = pacha_service_wait_init(
            &wait_set, (int)cfg->socket_endpoint_fd);
        if (status != 0)
            return netd_wait_set_failure(
                "service_wait_init", status, &wait_set);
        status = pacha_service_wait_add(
            &wait_set, timer_fd, PACHA_FD_EVENT_READABLE);
        if (status != 0)
            return netd_wait_set_failure("nic_timer", status, &wait_set);
        status = netd_libuinet_collect_runtime_wait_sources(&wait_set);
        if (status != 0)
            return netd_wait_set_failure(
                "libuinet_runtime", status, &wait_set);
        status = netd_socket_service_collect_wait_sources(&wait_set);
        if (status != 0)
            return netd_wait_set_failure(
                "socket_service", status, &wait_set);
        status = netd_netlink_socket_collect_wait_sources(&wait_set);
        if (status != 0)
            return netd_wait_set_failure(
                "netlink_socket", status, &wait_set);
        status = netd_libuinet_socket_collect_wait_sources(&wait_set);
        if (status != 0)
            return netd_wait_set_failure(
                "libuinet_socket", status, &wait_set);
        /* A continuously readable endpoint or a backed-up notification must
         * not starve lease/HANGUP reclamation. Poll once after every bounded
         * drain, and block only when there is no work left to pump. */
        (void)pacha_service_wait(&wait_set, busy ? 0 : PACHA_FD_WAIT_FOREVER);
        netd_socket_service_reap_hangups(&wait_set);
        netd_netlink_socket_reap_hangups(&wait_set);
        netd_libuinet_socket_reap_hangups(&wait_set);
    }
}
