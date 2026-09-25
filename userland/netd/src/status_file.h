/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdint.h>
#include <stddef.h>

struct netd_boot_config;
struct netd_dhcp_client;
struct netd_dhcp_lease;

void netd_status_file_bind_dhcp(const struct netd_dhcp_client *client);
void netd_status_file_publish(const struct netd_boot_config *cfg,
    const char *state, uint64_t stage, int status);
void netd_status_file_progress(const struct netd_boot_config *cfg,
    const char *phase);
void netd_status_file_note(const struct netd_boot_config *cfg,
    const char *phase);
int netd_policy_file_create(int filed_endpoint_fd,
    const struct netd_dhcp_lease *lease);
int netd_policy_file_read(int filed_endpoint_fd, char *text,
    size_t capacity, size_t *out_length);
int netd_resolver_file_create(int filed_endpoint_fd, const uint8_t dns[4]);
