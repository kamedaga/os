/* SPDX-License-Identifier: MIT */
#ifndef NETD_WIFI_CONTROL_H
#define NETD_WIFI_CONTROL_H
#include <stdint.h>
struct netd_runtime;
enum { NETD_WIFI_NETWORK_CONNECT = 1u, NETD_WIFI_NETWORK_DISCONNECT = 2u,
    NETD_WIFI_NETWORK_LOST = 4u };
/* The event-loop owner alone calls these, alongside its NIC/data-plane pump.
 * poll returns policy transitions; the caller retires the old IP policy
 * before pumping another frame and reports activation through ipv4_ready.
 */
int netd_wifi_control_start(struct netd_runtime *runtime);
unsigned netd_wifi_control_poll(struct netd_runtime *runtime, uint64_t now_ms);
void netd_wifi_control_ipv4_ready(int ready, int error);
/* Called by the 10 ms NIC info pump before receiving new-association frames.
 * It closes old-IP admission immediately and defers policy work to main. */
void netd_wifi_control_link_changed(int carrier);
unsigned netd_wifi_control_take_link_transition(void);
#endif
