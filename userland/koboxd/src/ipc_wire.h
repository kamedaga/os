#pragma once

#include "pacha/ipc.h"

#include <stdint.h>

extern const uint64_t koboxd_service_channel_rights;

int koboxd_recv_ipc_wait(int fd, struct pacha_ipc_msg *msg);
int koboxd_send_ipc_wait(int fd, const struct pacha_ipc_msg *msg);
int koboxd_send_endpoint_fd(int control_fd, uint64_t request_id, uint64_t endpoint_kind, int client_fd);
int koboxd_send_status_reply(int fd, uint64_t request_id, uint64_t word2);
int koboxd_send_status_reply_ex(int fd, uint64_t request_id, int64_t status, uint64_t result);
void *koboxd_map_wire_vmo_from_msg(const struct pacha_ipc_msg *request, uint64_t size, int *out_fd);
int koboxd_create_service_channel_pair(struct pacha_ipc_channel_pair *pair);
