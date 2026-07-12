#pragma once
#include <stddef.h>
#include <stdint.h>
#include "netd/ipc_protocol.h"

int netd_unix_socket_open(uint64_t type, uint64_t protocol, int notify_fd, uint64_t *out_handle);
int netd_unix_socket_attach_wait(uint64_t handle, int notify_fd);
int netd_unix_socket_bind(const netd_unix_path_t *req);
int netd_unix_socket_listen(uint64_t handle);
int netd_unix_socket_connect(const netd_unix_path_t *req);
int netd_unix_socket_accept(netd_accept_t *req);
int netd_unix_socket_send(const netd_io_t *req, int passed_wait_fd, size_t *out_sent);
int netd_unix_socket_recv(netd_io_t *req, size_t capacity, int *out_wait_fd, size_t *out_received);
int netd_unix_socket_poll(uint64_t handle, uint32_t events, uint32_t *out_revents, int32_t *out_error);
int netd_unix_socket_close(uint64_t handle);
int netd_unix_socket_is_handle(uint64_t handle);
