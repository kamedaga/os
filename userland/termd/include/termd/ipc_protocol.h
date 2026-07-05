#ifndef TERMD_IPC_PROTOCOL_H
#define TERMD_IPC_PROTOCOL_H

#include <stdint.h>

enum {
    TERMD_WIRE_REQUEST_MAGIC = 0x315152444d524554ull,
    TERMD_WIRE_REPLY_MAGIC = 0x315952444d524554ull,

    TERMD_WIRE_OP_HELLO = 1,
    TERMD_WIRE_OP_OPEN_PTMX = 2,
    TERMD_WIRE_OP_OPEN_PTS = 3,
    TERMD_WIRE_OP_OPEN_CTTY = 4,
    TERMD_WIRE_OP_CLOSE = 5,
    TERMD_WIRE_OP_READ = 6,
    TERMD_WIRE_OP_WRITE = 7,
    TERMD_WIRE_OP_IOCTL = 8,
    TERMD_WIRE_OP_POLL = 9,
    TERMD_WIRE_OP_DUP = 10,
    TERMD_WIRE_OP_TAKE_SIGNAL = 11,
    TERMD_WIRE_OP_OPEN_HVC = 12,

    TERMD_WIRE_PAGE_BYTES = 8192,
    TERMD_WIRE_IO_BYTES = TERMD_WIRE_PAGE_BYTES - 256,

    TERMD_WIRE_POLLIN = 0x0001,
    TERMD_WIRE_POLLOUT = 0x0004,
    TERMD_WIRE_POLLERR = 0x0008,
    TERMD_WIRE_POLLHUP = 0x0010,

    TERMD_WIRE_F_MASTER = 1u << 0,
    TERMD_WIRE_F_SLAVE = 1u << 1,
};

typedef struct termd_wire_open {
    uint64_t flags;
    uint64_t session_id;
    uint64_t process_id;
    uint64_t pgrp_id;
    uint64_t pts_index;
    uint64_t reserved[3];
} termd_wire_open_t;

typedef struct termd_wire_io {
    uint64_t handle;
    uint64_t length;
    uint64_t flags;
    uint64_t reserved[5];
    uint8_t data[TERMD_WIRE_IO_BYTES];
} termd_wire_io_t;

typedef struct termd_wire_ioctl {
    uint64_t handle;
    uint64_t request;
    uint64_t arg0;
    uint64_t arg1;
    uint64_t result0;
    uint64_t result1;
    uint64_t reserved[2];
    uint8_t data[256];
} termd_wire_ioctl_t;

typedef struct termd_wire_poll {
    uint64_t handle;
    uint32_t events;
    uint32_t revents;
    int32_t error;
    uint32_t reserved0;
    uint64_t reserved[5];
} termd_wire_poll_t;

typedef struct termd_wire_signal {
    uint64_t handle;
    uint32_t signo;
    uint32_t pgrp_id;
    uint64_t generation;
    uint64_t reserved[5];
} termd_wire_signal_t;

#endif
