#pragma once

#include <stdint.h>

/* Reply word1 is 0 or a negative Linux errno. */

enum {
    NETD_V2_REQUEST_MAGIC = 0x3251524454454eull,
    NETD_V2_REPLY_MAGIC = 0x3259524454454eull,

    NETD_V2_OP_HELLO = 0x0001u,
    NETD_V2_OP_SOCKET = 0x0101u,
    NETD_V2_OP_CONNECT = 0x0102u,
    NETD_V2_OP_CLOSE = 0x0103u,
    NETD_V2_OP_SEND = 0x0201u,
    NETD_V2_OP_RECV = 0x0202u,
    NETD_V2_OP_POLL = 0x0301u,

    NETD_V2_AF_INET = 2,
    NETD_V2_SOCK_STREAM = 1,
    NETD_V2_SOCK_DGRAM = 2,
    NETD_V2_IPPROTO_TCP = 6,
    NETD_V2_IPPROTO_UDP = 17,

    NETD_V2_PAGE_BYTES = 65536,
    NETD_V2_IO_BYTES = NETD_V2_PAGE_BYTES - 256,
    NETD_V2_POLLIN = 0x0001,
    NETD_V2_POLLOUT = 0x0004,
    NETD_V2_POLLERR = 0x0008,
};

typedef struct netd_v2_socket {
    uint64_t domain;
    uint64_t type;
    uint64_t protocol;
    uint64_t flags;
    uint64_t reserved[4];
} netd_v2_socket_t;

typedef struct netd_v2_sockaddr_in {
    uint32_t addr_be;
    uint16_t port_be;
    uint16_t reserved0;
    uint64_t reserved1;
} netd_v2_sockaddr_in_t;

typedef struct netd_v2_connect {
    uint64_t handle;
    uint64_t flags;
    netd_v2_sockaddr_in_t addr;
    uint64_t reserved[4];
} netd_v2_connect_t;

typedef struct netd_v2_io {
    uint64_t handle;
    uint64_t length;
    uint64_t flags;
    netd_v2_sockaddr_in_t addr;
    uint8_t data[NETD_V2_IO_BYTES];
} netd_v2_io_t;

typedef struct netd_v2_poll {
    uint64_t handle;
    uint32_t events;
    uint32_t revents;
    int32_t error;
    uint32_t reserved0;
    uint64_t reserved[5];
} netd_v2_poll_t;
