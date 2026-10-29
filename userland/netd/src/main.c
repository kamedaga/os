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
#include "wifi_control.h"
#include "pacha/capsule.h"
#include "pacha/ipc.h"
#include "pacha/log.h"
#include "pacha/bootstrap.h"

#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int validate_boot_config(const struct netd_boot_config *cfg)
{
    if (cfg == NULL ||
        cfg->magic != NETD_BOOT_CONFIG_MAGIC ||
        cfg->size != sizeof(*cfg) ||
        cfg->device_fd < 16 ||
        cfg->filed_endpoint_fd < 16 ||
        cfg->socket_endpoint_fd < 16 ||
        (cfg->status_channel_fd != 0 && cfg->status_channel_fd < 16) ||
        (cfg->log_endpoint_fd != 0 && cfg->log_endpoint_fd < 16) ||
        (cfg->flags & ~(NETD_BOOT_FLAG_SMOKE | NETD_BOOT_FLAG_TRACE | NETD_BOOT_FLAG_METRIC)) != 0) {
        if (cfg != NULL) {
            fprintf(stderr,
                "[netd] invalid boot config magic=0x%llx size=%llu fd=%llu socket_fd=%llu\n",
                (unsigned long long)cfg->magic,
                (unsigned long long)cfg->size,
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
    if (cfg->log_endpoint_fd >= 16) {
        char line[128];
        const struct netd_nic_diagnostic *diagnostic =
            netd_kobox2_nic_diagnostic();
        int size = snprintf(line, sizeof(line),
            "network stage=%llu status=%d nic_step=%u detail=%d loaded=%u pci_bound=%u",
            (unsigned long long)stage, status, diagnostic->step,
            diagnostic->detail, diagnostic->loaded, diagnostic->pci_bound);
        if (size > 0 && (size_t)size < sizeof(line))
            (void)pacha_log_emit((pacha_fd_t)cfg->log_endpoint_fd,
                PACHA_LOG_SOURCE_NETD, line, (size_t)size);
    }
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
    return pacha_ipc_send((pacha_fd_t)cfg->status_channel_fd, &report);
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
static int netd_activate_ipv4(struct netd_runtime *runtime, int *state,
    const struct netd_ipv4_boot_config *selected)
{
    if (!runtime || !runtime->cfg || !state || !selected) return -EINVAL;
    const struct netd_boot_config *cfg = runtime->cfg;
    const struct netd_ipv4_boot_config ipv4 = *selected;
    if (netd_network_config_init(&ipv4) != 0) {
        return -EINVAL;
    }
    int status = netd_upper_start(runtime);
    if (!status && (cfg->flags & NETD_BOOT_FLAG_SMOKE) != 0)
        status = netd_packet_io_smoke_after_ipv4();
    if (status != 0) {
        fprintf(stderr, "[netd] IPv4 stack failed status=%d; NIC remains active\n", status);
        *state = -1;
        netd_wifi_control_ipv4_ready(0, status < 0 ? status : -EIO);
        return status < 0 ? status : -EIO;
    }
    *state = 1;
    printf("[netd] IPv4 policy applied after NIC startup\n");
    /* NIC readiness precedes DHCP and libuinet startup. Publish a separate
     * readiness event so dependent services cannot race the TCP/IP stack. */
    (void)report_link_status(cfg, 0, NETD_BOOT_STAGE_IPV4);
    int resolver_status = netd_resolver_file_create(
        (pacha_fd_t)cfg->filed_endpoint_fd, ipv4.dns);
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
    netd_wifi_control_ipv4_ready(1, 0);
    fflush(stdout);
    return 0;
}

static int netd_try_ipv4_policy(struct netd_runtime *runtime, int *state)
{
    static int last_policy_error;
    if (!runtime || !runtime->cfg || !state || *state != 0) return 0;
    const struct netd_boot_config *cfg = runtime->cfg;
    struct netd_ipv4_boot_config ipv4 = cfg->ipv4;
    if (!(ipv4.address[0] | ipv4.address[1] | ipv4.address[2] | ipv4.address[3])) {
        char text[512];
        size_t length;
        int error = netd_policy_file_read((pacha_fd_t)cfg->filed_endpoint_fd,
            text, sizeof(text), &length);
        if (error) {
            if (error != -ENOENT && error != last_policy_error)
                fprintf(stderr, "[netd] IPv4 policy read failed status=%d\n", error);
            last_policy_error = error;
            return 0;
        }
        /* A cached DHCP file does not prove this LAN still leased that IP.
         * Fresh ACKs are applied directly; only manual files override DHCP. */
        if (netd_policy_file_is_generated(text)) return 0;
        if (netd_network_config_parse_text(text, &ipv4)) {
            if (last_policy_error != -EINVAL)
                netd_status_file_note(cfg, "IPv4 policy parse failed; inspect /run/pacha/network.conf");
            last_policy_error = -EINVAL;
            return 0;
        }
    }
    last_policy_error = 0;
    return netd_activate_ipv4(runtime, state, &ipv4);
}

static int netd_wifi_policy_transition(struct netd_runtime *runtime, unsigned action,
    int *state, int *dhcp_mode, int *hold, unsigned *attempted_ack)
{
    if (!action) return 0;
    netd_libuinet_ipv4_suspend();
    netd_packet_io_discard_rx();
    netd_wifi_control_ipv4_ready(0, 0);
    netd_packet_io_dhcp_disable();
    *state = 0;
    *attempted_ack = 0;
    *hold = !(action & NETD_WIFI_NETWORK_CONNECT);
    if (*hold) return 0;
    int error = netd_try_ipv4_policy(runtime, state);
    if (!error && *state == 1) { *dhcp_mode = 0; return 0; }
    if (!error) {
        *dhcp_mode = 1;
        error = netd_packet_io_dhcp_restart(runtime);
    }
    if (error) netd_wifi_control_ipv4_ready(0, error < 0 ? error : -EIO);
    return error;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)setenv("KOBOX_DAEMON_NAME", "netd", 1);
    struct netd_boot_config config;
    memset(&config, 0, sizeof(config));
    const pacha_fd_t bootstrap_fd = pacha_bootstrap_fd_from_argv(argv);
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
    int wifi_backend = 0;
    if (netd_kobox2_wifi_generation()) {
        int error = netd_wifi_control_start(&runtime);
        if (!error) wifi_backend = 1;
        else fprintf(stderr, "[netd] Wi-Fi control initialization failed status=%d\n", error);
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
    int dhcp_mode = ipv4_state == 0, wifi_hold = 0;
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
    pacha_fd_t timer_fd = pacha_timerfd_create(UINT64_C(10000000),
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
    uint64_t wifi_ticks = 0;
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
            wifi_ticks += expirations;
            if (wifi_ticks >= 100) {
                wifi_ticks %= 100;
                if (!wifi_backend && netd_kobox2_wifi_generation())
                    wifi_backend = netd_wifi_control_start(&runtime) == 0;
                struct timespec now;
                if (wifi_backend && !clock_gettime(CLOCK_MONOTONIC, &now) &&
                    now.tv_sec >= 0 && now.tv_nsec >= 0) {
                    unsigned action = netd_wifi_control_poll(&runtime,
                        (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000);
                    if (action) {
                        (void)netd_wifi_policy_transition(&runtime, action, &ipv4_state,
                            &dhcp_mode, &wifi_hold, &policy_attempted_ack);
                        policy_ticks = 0;
                    }
                }
            }
            if (ipv4_state == 0 && !wifi_hold) {
                policy_ticks += expirations;
                if (policy_ticks >= 100) {
                    policy_ticks %= 100;
                    (void)netd_try_ipv4_policy(&runtime, &ipv4_state);
                    if (ipv4_state == 1 && !dhcp_owned) {
                        netd_packet_io_dhcp_disable();
                        dhcp_mode = 0;
                    }
                }
            }
        }
        netd_packet_io_pump_once();
        unsigned link_action = netd_wifi_control_take_link_transition();
        if (link_action) {
            (void)netd_wifi_policy_transition(&runtime, link_action, &ipv4_state,
                &dhcp_mode, &wifi_hold, &policy_attempted_ack);
            policy_ticks = 0;
        }
        const struct netd_dhcp_client *dhcp = netd_packet_io_dhcp_status();
        if (dhcp_mode && !wifi_hold && dhcp != NULL &&
            dhcp->state == NETD_DHCP_BOUND &&
            policy_attempted_ack != dhcp->acks) {
            policy_attempted_ack = dhcp->acks;
            int policy_status = netd_policy_file_create(
                (pacha_fd_t)cfg->filed_endpoint_fd, &dhcp->lease);
            if (policy_status == 0) {
                dhcp_owned = 1;
                struct netd_ipv4_boot_config leased = {0};
                memcpy(leased.address, dhcp->lease.address, 4);
                memcpy(leased.netmask, dhcp->lease.netmask, 4);
                memcpy(leased.gateway, dhcp->lease.gateway, 4);
                memcpy(leased.dns, dhcp->lease.dns, 4);
                (void)netd_activate_ipv4(&runtime, &ipv4_state, &leased);
                netd_status_file_note(cfg,
                    "DHCP lease committed to /run/pacha/network.conf");
            } else if (policy_status != -EEXIST) {
                char phase[80];
                (void)snprintf(phase, sizeof(phase),
                    "DHCP policy write failed status=%d", policy_status);
                netd_status_file_note(cfg, phase);
                netd_wifi_control_ipv4_ready(0, policy_status);
            }
            if (policy_status == -EEXIST) {
                dhcp_owned = 0;
                ipv4_state = 0;
                netd_libuinet_ipv4_suspend();
                netd_wifi_control_ipv4_ready(0, 0);
                (void)netd_try_ipv4_policy(&runtime, &ipv4_state);
            }
            if (ipv4_state == 1 && !dhcp_owned) {
                netd_packet_io_dhcp_disable();
                dhcp_mode = 0;
            }
        }
        if (dhcp_mode && ipv4_state == 1 && dhcp &&
            dhcp->state != NETD_DHCP_BOUND && dhcp->state != NETD_DHCP_RENEWING &&
            dhcp->state != NETD_DHCP_REBINDING) {
            netd_libuinet_ipv4_suspend();
            netd_wifi_control_ipv4_ready(0, 0);
            ipv4_state = 0;
        }
        const struct netd_link_info *link = netd_link_current();
        if (link && (int)link->carrier != last_reported_carrier &&
            report_link_status(cfg, 0, NETD_BOOT_STAGE_LINK) == 0)
            last_reported_carrier = link->carrier;
        const int busy = netd_socket_service_poll();
        static struct pacha_service_wait_set wait_set;
        static struct pacha_fd_wait_retained retained_wait;
        status = pacha_service_wait_init(
            &wait_set, (pacha_fd_t)cfg->socket_endpoint_fd);
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
        (void)pacha_service_wait_retained(&wait_set, &retained_wait, busy ? 0 : PACHA_FD_WAIT_FOREVER);
        netd_socket_service_reap_hangups(&wait_set);
        netd_netlink_socket_reap_hangups(&wait_set);
        netd_libuinet_socket_reap_hangups(&wait_set);
    }
}
