#pragma once

#include "netd_internal.h"

int netd_socket_service_start(struct netd_runtime *runtime);
void netd_socket_service_poll(void);

