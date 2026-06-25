#pragma once

#include "block_service.h"
#include "ipc_service.h"

#include <stdint.h>

int koboxd_control_serve_get_endpoint(
    koboxd_ipc_service_t *ipc_service,
    int control_fd,
    uint64_t expected_kind);
int koboxd_block_serve_identify(
    koboxd_ipc_service_t *ipc_service,
    const koboxd_block_service_t *block_service);
