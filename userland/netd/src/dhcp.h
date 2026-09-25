/* SPDX-License-Identifier: MIT */
#pragma once

#include <stddef.h>
#include <stdint.h>

enum netd_dhcp_state {
    NETD_DHCP_WAIT_LINK,
    NETD_DHCP_SELECTING,
    NETD_DHCP_REQUESTING,
    NETD_DHCP_BOUND,
    NETD_DHCP_RENEWING,
    NETD_DHCP_REBINDING,
    NETD_DHCP_EXPIRED,
    NETD_DHCP_DISABLED,
};

struct netd_dhcp_lease {
    uint8_t address[4];
    uint8_t netmask[4];
    uint8_t gateway[4];
    uint8_t dns[4];
    uint8_t server[4];
    uint32_t seconds;
    uint32_t renew_seconds;
    uint32_t rebind_seconds;
};

typedef int (*netd_dhcp_send_fn)(void *context, const void *frame,
    size_t length);
typedef void (*netd_dhcp_event_fn)(void *context, const char *event,
    int detail);

struct netd_dhcp_client {
    enum netd_dhcp_state state;
    uint8_t mac[6];
    uint8_t server_mac[6];
    uint32_t xid;
    uint32_t generation;
    uint32_t attempts;
    uint32_t offers;
    uint32_t acks;
    uint32_t rejected;
    int last_error;
    uint64_t next_send_ms;
    uint64_t renew_ms;
    uint64_t rebind_ms;
    uint64_t expire_ms;
    struct netd_dhcp_lease offer;
    struct netd_dhcp_lease lease;
    netd_dhcp_send_fn send;
    netd_dhcp_event_fn event;
    void *context;
};

void netd_dhcp_init(struct netd_dhcp_client *client, const uint8_t mac[6],
    uint32_t xid, netd_dhcp_send_fn send, netd_dhcp_event_fn event,
    void *context);
void netd_dhcp_disable(struct netd_dhcp_client *client);
void netd_dhcp_poll(struct netd_dhcp_client *client, uint64_t now_ms,
    int carrier);
/* Returns 1 for a DHCP reply addressed to this client, 0 for other frames. */
int netd_dhcp_receive(struct netd_dhcp_client *client, const void *frame,
    size_t length, uint64_t now_ms);
const char *netd_dhcp_state_name(enum netd_dhcp_state state);
