#pragma once

#include <stdint.h>

#include "pacha/service_abi.h"

/* Reply word1 is 0 or a negative Linux errno. */

enum {
    NETD_OP_HELLO = 0u,
    NETD_OP_SOCKET = 1u,
    NETD_OP_CONNECT = 2u,
    NETD_OP_CLOSE = 3u,
    NETD_OP_SEND = 4u,
    NETD_OP_RECV = 5u,
    NETD_OP_POLL = 6u,

    NETD_AF_INET = 2,
    NETD_SOCK_STREAM = 1,
    NETD_SOCK_DGRAM = 2,
    NETD_IPPROTO_TCP = 6,
    NETD_IPPROTO_UDP = 17,

    NETD_PAGE_BYTES = 65536,
    NETD_IO_BYTES = NETD_PAGE_BYTES - 256,
    NETD_POLLIN = 0x0001,
    NETD_POLLOUT = 0x0004,
    NETD_POLLERR = 0x0008,
};

typedef struct netd_socket {
    uint64_t domain;
    uint64_t type;
    uint64_t protocol;
    uint64_t flags;
    uint64_t reserved[4];
} netd_socket_t;

typedef struct netd_sockaddr_in {
    uint32_t addr_be;
    uint16_t port_be;
    uint16_t reserved0;
    uint64_t reserved1;
} netd_sockaddr_in_t;

typedef struct netd_connect {
    uint64_t handle;
    uint64_t flags;
    netd_sockaddr_in_t addr;
    uint64_t reserved[4];
} netd_connect_t;

typedef struct netd_io {
    uint64_t handle;
    uint64_t length;
    uint64_t flags;
    netd_sockaddr_in_t addr;
    uint8_t data[NETD_IO_BYTES];
} netd_io_t;

typedef struct netd_poll {
    uint64_t handle;
    uint32_t events;
    uint32_t revents;
    int32_t error;
    uint32_t reserved0;
    uint64_t reserved[5];
} netd_poll_t;
