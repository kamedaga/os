#pragma once

#include <stddef.h>
#include <stdint.h>

#include "netd/ipc_protocol.h"

struct pacha_service_wait_set;

int netd_netlink_socket_open(uint64_t type, uint64_t protocol, int notify_fd, uint64_t *out_handle);
int netd_netlink_socket_dup(uint64_t handle);
int netd_netlink_socket_bind(const netd_netlink_bind_t *request);
int netd_netlink_socket_recv(netd_io_t *request, size_t capacity, size_t *out_received);
int netd_netlink_socket_poll(uint64_t handle, uint32_t events, uint32_t *out_revents, int32_t *out_error);
int netd_netlink_socket_close(uint64_t handle);
int netd_netlink_socket_is_handle(uint64_t handle);
int netd_netlink_publish_device(uint64_t device);
int netd_netlink_socket_collect_wait_sources(struct pacha_service_wait_set *wait_set);
void netd_netlink_socket_reap_hangups(
    const struct pacha_service_wait_set *wait_set);
