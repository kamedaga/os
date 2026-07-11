#pragma once

#include <stdint.h>

#include "filed/ipc_protocol.h"
#include "filed/payload.h"
#include "lpr_supervisor/ipc_protocol.h"
#include "netd/ipc_protocol.h"
#include "pacha/service_abi.h"
#include "termd/ipc_protocol.h"

enum {
    LPR_CLIENT_ABI_VERSION = PACHA_SERVICE_ABI_VERSION,
    LPR_CLIENT_MAX_IOV = 16u,
    LPR_CLIENT_MAX_PATH = 480u,

    LPR_CLIENT_FD_KIND_EMPTY = 0u,
    LPR_CLIENT_FD_KIND_FILED_HANDLE = 1u,
    LPR_CLIENT_FD_KIND_TERMD_HANDLE = 2u,
    LPR_CLIENT_FD_KIND_DRMD_HANDLE = 3u,
    LPR_CLIENT_FD_KIND_PIPE = 4u,
    LPR_CLIENT_FD_KIND_EVENT = 5u,
    LPR_CLIENT_FD_KIND_SOCKET = 6u,

    LPR_CLIENT_FD_CLOEXEC = 1u << 0,
    LPR_CLIENT_FD_NONBLOCK = 1u << 1,
    LPR_CLIENT_FD_APPEND = 1u << 2,
};

typedef struct lpr_client_result {
    int64_t status;
    uint32_t error_domain;
    uint32_t reserved0;
    uint64_t result;
    uint64_t trace_id;
} lpr_client_result_t;

typedef struct lpr_client_fd_ref {
    int32_t linux_fd;
    int32_t service_fd;
    uint32_t kind;
    uint32_t flags;
    uint64_t service_handle;
    uint64_t rights;
} lpr_client_fd_ref_t;

typedef struct lpr_client_path_request {
    int32_t dirfd;
    uint32_t flags;
    uint64_t mode;
    char path[LPR_CLIENT_MAX_PATH];
} lpr_client_path_request_t;

typedef struct lpr_client_io_request {
    int32_t linux_fd;
    uint32_t flags;
    uint64_t offset;
    uint64_t length;
    uint64_t buffer;
} lpr_client_io_request_t;

typedef struct lpr_client_tty_context {
    uint64_t session_id;
    uint64_t process_id;
    uint64_t pgrp_id;
    uint64_t signal_mask;
    uint64_t signal_ignored;
} lpr_client_tty_context_t;

_Static_assert(sizeof(lpr_client_result_t) == 32, "lpr_client_result size");
_Static_assert(sizeof(lpr_client_fd_ref_t) == 32, "lpr_client_fd_ref size");
_Static_assert(sizeof(lpr_client_path_request_t) == 496, "lpr_client_path_request size");
_Static_assert(sizeof(lpr_client_io_request_t) == 32, "lpr_client_io_request size");
_Static_assert(sizeof(lpr_client_tty_context_t) == 40, "lpr_client_tty_context size");
