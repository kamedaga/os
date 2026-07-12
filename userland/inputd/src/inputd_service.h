#pragma once

#include "input_island.h"

#include <pacha/ipc.h>

typedef struct inputd_service {
    const struct inputd_boot_config *cfg;
    struct inputd_input_island *input;
} inputd_service_t;

int inputd_service_send_boot_ready(inputd_service_t *service, int64_t status, uint64_t result);
int inputd_service_dispatch(
    inputd_service_t *service,
    const struct pacha_ipc_msg *request,
    const struct pacha_ipc_fd *fds);
