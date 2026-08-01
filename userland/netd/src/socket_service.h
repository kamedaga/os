#pragma once

#include "netd_internal.h"

struct pacha_service_wait_set;

int netd_socket_service_start(struct netd_runtime *runtime);
void netd_socket_service_poll(void);
int netd_socket_service_collect_wait_sources(struct pacha_service_wait_set *wait_set);
void netd_socket_service_reap_hangups(
    const struct pacha_service_wait_set *wait_set);
