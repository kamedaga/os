#pragma once

#include <stdint.h>

#include "pacha/service_abi.h"

/* Replies use pacha_service_envelope.status: 0 or a negative Linux errno. */

enum {
    TERMD_SERVICE_ID = PACHA_SERVICE_ID_TERMD,

    TERMD_OP_HELLO = 0u,
    TERMD_OP_OPEN_PTMX = 1u,
    TERMD_OP_OPEN_PTS = 2u,
    TERMD_OP_OPEN_CTTY = 3u,
    TERMD_OP_OPEN_HVC = 4u,
    TERMD_OP_HANDLE_CLOSE = 5u,
    TERMD_OP_HANDLE_DUP = 6u,
    TERMD_OP_HANDLE_READ = 7u,
    TERMD_OP_HANDLE_WRITE = 8u,
    TERMD_OP_HANDLE_IOCTL = 9u,
    TERMD_OP_HANDLE_POLL = 10u,
    TERMD_OP_SIGNAL_TAKE = 11u,
    TERMD_OP_SIGNAL_REGISTER_SUPERVISOR = 12u,
    TERMD_OP_DIAG_DUMP = 13u,
    TERMD_OP_DIAG_ERROR_GET = 14u,

    TERMD_PAGE_BYTES = PACHA_SERVICE_PAGE_BYTES,
    TERMD_IO_BYTES = 7936u,
    TERMD_IOCTL_DATA_BYTES = 256u,

    TERMD_POLLIN = 0x0001u,
    TERMD_POLLOUT = 0x0004u,
    TERMD_POLLERR = 0x0008u,
    TERMD_POLLHUP = 0x0010u,

    /* The service must execute this I/O attempt without sleeping.  The LPR
     * caller implements the guest-visible blocking/nonblocking semantics. */
    TERMD_IO_F_NOWAIT = 1u << 0,

    TERMD_F_MASTER = 1u << 0,
    TERMD_F_SLAVE = 1u << 1,

    /* Linux TIOCGPTPEER returns a process-local fd.  Across the termd
     * boundary its successful result is instead a termd handle which LPR
     * materializes as a Linux-visible fd. */
    TERMD_IOCTL_TIOCGPTPEER = 0x5441u,
};

typedef struct termd_tty_context {
    uint64_t session_id;
    uint64_t process_id;
    uint64_t pgrp_id;
    uint64_t signal_mask;
    uint64_t signal_ignored;
} termd_tty_context_t;

typedef struct termd_open_request {
    termd_tty_context_t tty;
    uint64_t flags;
    uint64_t pts_index;
} termd_open_request_t;

typedef struct termd_handle_request {
    termd_tty_context_t tty;
    uint64_t handle;
    uint64_t arg0;
    uint64_t arg1;
    uint64_t arg2;
} termd_handle_request_t;

typedef struct termd_io_request {
    termd_tty_context_t tty;
    uint64_t handle;
    uint64_t length;
    uint64_t flags;
    uint8_t data[TERMD_IO_BYTES];
} termd_io_request_t;

typedef struct termd_ioctl_request {
    termd_tty_context_t tty;
    uint64_t handle;
    uint64_t request;
    uint64_t arg0;
    uint64_t arg1;
    uint64_t result0;
    uint64_t result1;
    uint8_t data[TERMD_IOCTL_DATA_BYTES];
} termd_ioctl_request_t;

typedef struct termd_poll_request {
    termd_tty_context_t tty;
    uint64_t handle;
    uint32_t events;
    uint32_t revents;
    int32_t error;
    uint32_t reserved0;
} termd_poll_request_t;

typedef struct termd_signal_request {
    uint64_t handle;
    uint32_t signo;
    uint32_t pgrp_id;
    uint64_t generation;
    uint64_t flags;
} termd_signal_request_t;

_Static_assert(sizeof(termd_tty_context_t) == 40, "termd_tty_context size");
_Static_assert(sizeof(termd_open_request_t) == 56, "termd_open_request size");
_Static_assert(sizeof(termd_handle_request_t) == 72, "termd_handle_request size");
_Static_assert(sizeof(termd_io_request_t) == 8000, "termd_io_request size");
_Static_assert(sizeof(termd_ioctl_request_t) == 344, "termd_ioctl_request size");
_Static_assert(sizeof(termd_poll_request_t) == 64, "termd_poll_request size");
_Static_assert(sizeof(termd_signal_request_t) == 32, "termd_signal_request size");
