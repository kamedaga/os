#pragma once

#include "fs_backend.h"
#include "ipc_service.h"

int koboxd_fs_endpoint_serve_once(
    koboxd_ipc_service_t *ipc_service,
    koboxd_fs_backend_t *fs_backend);
