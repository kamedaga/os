#pragma once

#include <stdint.h>

enum {
    NETD_WIRE_REQUEST_MAGIC = 0x3151524454454eull,
    NETD_WIRE_REPLY_MAGIC = 0x3159524454454eull,

    NETD_WIRE_OP_HELLO = 1,
    NETD_WIRE_OP_SOCKET = 2,
    NETD_WIRE_OP_CONNECT = 3,
    NETD_WIRE_OP_SEND = 4,
    NETD_WIRE_OP_RECV = 5,
    NETD_WIRE_OP_CLOSE = 6,
    NETD_WIRE_OP_POLL = 7,

    NETD_WIRE_AF_INET = 2,
    NETD_WIRE_SOCK_STREAM = 1,
    NETD_WIRE_SOCK_DGRAM = 2,
    NETD_WIRE_IPPROTO_TCP = 6,
    NETD_WIRE_IPPROTO_UDP = 17,

    NETD_WIRE_PAGE_BYTES = 65536,
    NETD_WIRE_IO_BYTES = NETD_WIRE_PAGE_BYTES - 256,
    NETD_WIRE_POLLIN = 0x0001,
    NETD_WIRE_POLLOUT = 0x0004,
    NETD_WIRE_POLLERR = 0x0008,
};

typedef struct netd_wire_socket {
    uint64_t domain;
    uint64_t type;
    uint64_t protocol;
    uint64_t flags;
    uint64_t reserved[4];
} netd_wire_socket_t;

typedef struct netd_wire_sockaddr_in {
    uint32_t addr_be;
    uint16_t port_be;
    uint16_t reserved0;
    uint64_t reserved1;
} netd_wire_sockaddr_in_t;

typedef struct netd_wire_connect {
    uint64_t handle;
    uint64_t flags;
    netd_wire_sockaddr_in_t addr;
    uint64_t reserved[4];
} netd_wire_connect_t;

typedef struct netd_wire_io {
    uint64_t handle;
    uint64_t length;
    uint64_t flags;
    netd_wire_sockaddr_in_t addr;
    uint8_t data[NETD_WIRE_IO_BYTES];
} netd_wire_io_t;

typedef struct netd_wire_poll {
    uint64_t handle;
    uint32_t events;
    uint32_t revents;
    int32_t error;
    uint32_t reserved0;
    uint64_t reserved[5];
} netd_wire_poll_t;
