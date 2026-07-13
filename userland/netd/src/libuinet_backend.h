#pragma once

#include "netd_internal.h"
#include "upper.h"

#include <stdint.h>

enum netd_libuinet_state {
    NETD_LIBUINET_UNLINKED = 0,
    NETD_LIBUINET_READY = 1,
    NETD_LIBUINET_ERROR = 2,
};

int netd_libuinet_start(struct netd_runtime *runtime);
int netd_libuinet_receive_frame(const struct netd_upper_frame *frame);
void netd_libuinet_poll(void);
int netd_libuinet_needs_periodic_poll(void);
enum netd_libuinet_state netd_libuinet_state(void);
uint64_t netd_libuinet_rx_frames(void);
uint64_t netd_libuinet_rx_drops(void);

int netd_libuinet_socket_open(uint64_t domain, uint64_t type, uint64_t protocol, uint64_t *out_handle);
int netd_libuinet_socket_connect(uint64_t handle, uint32_t addr_be, uint16_t port_be, uint64_t flags);
int netd_libuinet_socket_send(uint64_t handle, const void *data, size_t len, uint64_t flags, uint32_t addr_be, uint16_t port_be, size_t *out_sent);
int netd_libuinet_socket_recv(uint64_t handle, void *data, size_t capacity, uint64_t flags, size_t *out_received);
int netd_libuinet_socket_poll(uint64_t handle, uint32_t events, uint32_t *out_revents, int32_t *out_error);
int netd_libuinet_socket_close(uint64_t handle);
int netd_filed_close_handle(uint64_t handle);
