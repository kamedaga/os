/* SPDX-License-Identifier: MIT */
#ifndef PACHA_NETD_NETWORK_CONFIG_H
#define PACHA_NETD_NETWORK_CONFIG_H

#include <netd/boot_config.h>

struct netd_network_config {
    uint8_t address[4];
    uint8_t netmask[4];
    uint8_t broadcast[4];
    uint8_t gateway[4];
    uint8_t dns[4];
    uint8_t link_mac[6];
    char address_text[16];
    char mask_text[16];
    char broadcast_text[16];
    char gateway_text[16];
    char dns_text[16];
};

/* Link identity is independent of IPv4 policy. The NIC may attach first;
 * a policy can be supplied from RAM later without losing the NIC's MAC. */
int netd_network_config_init(const struct netd_ipv4_boot_config *boot);
int netd_network_config_parse_text(const char *text,
    struct netd_ipv4_boot_config *out);
int netd_network_config_set_link_mac(const uint8_t mac[6]);
const struct netd_network_config *netd_network_config_get(void);

#endif
