#pragma once

#include <stdint.h>

#include "pacha/service_abi.h"

enum {
    TERMD_V2_SERVICE_ID = PACHA_SERVICE_ID_TERMD,

    TERMD_V2_OP_HELLO = 0x0001u,
    TERMD_V2_OP_OPEN_PTMX = 0x0101u,
    TERMD_V2_OP_OPEN_PTS = 0x0102u,
    TERMD_V2_OP_OPEN_CTTY = 0x0103u,
    TERMD_V2_OP_OPEN_HVC = 0x0104u,
    TERMD_V2_OP_HANDLE_CLOSE = 0x0201u,
    TERMD_V2_OP_HANDLE_DUP = 0x0202u,
    TERMD_V2_OP_HANDLE_READ = 0x0301u,
    TERMD_V2_OP_HANDLE_WRITE = 0x0302u,
    TERMD_V2_OP_HANDLE_IOCTL = 0x0303u,
    TERMD_V2_OP_HANDLE_POLL = 0x0304u,
    TERMD_V2_OP_SIGNAL_TAKE = 0x0401u,
    TERMD_V2_OP_SIGNAL_REGISTER_SUPERVISOR = 0x0402u,
    TERMD_V2_OP_DIAG_DUMP = 0x7f01u,
    TERMD_V2_OP_DIAG_ERROR_GET = 0x7f02u,

    TERMD_V2_PAGE_BYTES = PACHA_SERVICE_PAGE_BYTES,
    TERMD_V2_IO_BYTES = 7936u,
    TERMD_V2_IOCTL_DATA_BYTES = 256u,

    TERMD_V2_POLLIN = 0x0001u,
    TERMD_V2_POLLOUT = 0x0004u,
    TERMD_V2_POLLERR = 0x0008u,
    TERMD_V2_POLLHUP = 0x0010u,

    TERMD_V2_F_MASTER = 1u << 0,
    TERMD_V2_F_SLAVE = 1u << 1,
};

typedef struct termd_v2_tty_context {
    uint64_t session_id;
    uint64_t process_id;
    uint64_t pgrp_id;
    uint64_t signal_mask;
    uint64_t signal_ignored;
} termd_v2_tty_context_t;

typedef struct termd_v2_open_request {
    termd_v2_tty_context_t tty;
    uint64_t flags;
    uint64_t pts_index;
} termd_v2_open_request_t;

typedef struct termd_v2_handle_request {
    termd_v2_tty_context_t tty;
    uint64_t handle;
    uint64_t arg0;
    uint64_t arg1;
    uint64_t arg2;
} termd_v2_handle_request_t;

typedef struct termd_v2_io_request {
    termd_v2_tty_context_t tty;
    uint64_t handle;
    uint64_t length;
    uint64_t flags;
    uint8_t data[TERMD_V2_IO_BYTES];
} termd_v2_io_request_t;

typedef struct termd_v2_ioctl_request {
    termd_v2_tty_context_t tty;
    uint64_t handle;
    uint64_t request;
    uint64_t arg0;
    uint64_t arg1;
    uint64_t result0;
    uint64_t result1;
    uint8_t data[TERMD_V2_IOCTL_DATA_BYTES];
} termd_v2_ioctl_request_t;

typedef struct termd_v2_poll_request {
    termd_v2_tty_context_t tty;
    uint64_t handle;
    uint32_t events;
    uint32_t revents;
    int32_t error;
    uint32_t reserved0;
} termd_v2_poll_request_t;

typedef struct termd_v2_signal_request {
    uint64_t handle;
    uint32_t signo;
    uint32_t pgrp_id;
    uint64_t generation;
    uint64_t flags;
} termd_v2_signal_request_t;

_Static_assert(sizeof(termd_v2_tty_context_t) == 40, "termd_v2_tty_context size");
_Static_assert(sizeof(termd_v2_open_request_t) == 56, "termd_v2_open_request size");
_Static_assert(sizeof(termd_v2_handle_request_t) == 72, "termd_v2_handle_request size");
_Static_assert(sizeof(termd_v2_io_request_t) == 8000, "termd_v2_io_request size");
_Static_assert(sizeof(termd_v2_ioctl_request_t) == 344, "termd_v2_ioctl_request size");
_Static_assert(sizeof(termd_v2_poll_request_t) == 64, "termd_v2_poll_request size");
_Static_assert(sizeof(termd_v2_signal_request_t) == 32, "termd_v2_signal_request size");
