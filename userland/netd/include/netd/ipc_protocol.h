#pragma once

#include <stdint.h>

#include "pacha/service_abi.h"

/* Reply word1 is 0 or a negative Linux errno. */

enum {
    NETD_OP_HELLO = 0u,
    NETD_OP_PAGE_ATTACH = 1u,
    NETD_OP_SOCKET = 2u,
    NETD_OP_SOCKETPAIR = 3u,
    NETD_OP_CONNECT = 4u,
    NETD_OP_CLOSE = 5u,
    NETD_OP_SEND = 6u,
    NETD_OP_RECV = 7u,
    NETD_OP_POLL = 8u,
    NETD_OP_BIND = 9u,
    NETD_OP_LISTEN = 10u,
    NETD_OP_ACCEPT = 11u,
    NETD_OP_ATTACH_WAIT = 12u,
    NETD_OP_UEVENT_PUBLISH = 13u,
    NETD_OP_DUP = 14u,

    NETD_STATUS_STALE_ATTACHMENT = -116,

    NETD_AF_UNIX = 1,
    NETD_AF_INET = 2,
    NETD_AF_NETLINK = 16,
    NETD_SOCK_STREAM = 1,
    NETD_SOCK_DGRAM = 2,
    NETD_SOCK_RAW = 3,
    NETD_SOCK_SEQPACKET = 5,
    NETD_IPPROTO_TCP = 6,
    NETD_IPPROTO_UDP = 17,
    NETD_NETLINK_KOBJECT_UEVENT = 15,

    NETD_UEVENT_DRM_CARD0 = 1,

    NETD_INPUT_CAP_KEYBOARD = 1u << 0,
    NETD_INPUT_CAP_RELATIVE = 1u << 1,
    NETD_INPUT_CAP_ABSOLUTE = 1u << 2,

    NETD_PAGE_BYTES = 65536,
    NETD_IO_BYTES = NETD_PAGE_BYTES - 1024,
    NETD_TRANSFER_MAX_ITEMS = 16,
    NETD_TRANSFER_MAX_CAPABILITIES = 16,
    NETD_POLLIN = 0x0001,
    NETD_POLLOUT = 0x0004,
    NETD_POLLERR = 0x0008,
    NETD_POLLHUP = 0x0010,
};

#define NETD_UEVENT_INPUT_TAG (UINT64_C(1) << 63)
#define NETD_UEVENT_INPUT_RESERVED_MASK UINT64_C(0x7f00000000000000)
#define NETD_UEVENT_INPUT_CAPABILITIES_SHIFT 48u
#define NETD_UEVENT_INPUT_EVENT_INDEX_SHIFT 32u

typedef struct netd_input_uevent_descriptor {
    uint16_t segment;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t capabilities;
    uint16_t event_index;
} netd_input_uevent_descriptor_t;

static inline uint64_t netd_input_uevent_encode(netd_input_uevent_descriptor_t descriptor)
{
    const uint32_t bdf =
        ((uint32_t)descriptor.segment << 16u) |
        ((uint32_t)descriptor.bus << 8u) |
        (((uint32_t)descriptor.device & 0x1fu) << 3u) |
        ((uint32_t)descriptor.function & 0x07u);
    return NETD_UEVENT_INPUT_TAG |
        ((uint64_t)descriptor.capabilities << NETD_UEVENT_INPUT_CAPABILITIES_SHIFT) |
        ((uint64_t)descriptor.event_index << NETD_UEVENT_INPUT_EVENT_INDEX_SHIFT) |
        bdf;
}

static inline int netd_input_uevent_decode(
    uint64_t encoded,
    netd_input_uevent_descriptor_t *out_descriptor)
{
    if (out_descriptor == 0 || (encoded & NETD_UEVENT_INPUT_TAG) == 0 ||
        (encoded & NETD_UEVENT_INPUT_RESERVED_MASK) != 0)
        return 0;
    const uint32_t bdf = (uint32_t)encoded;
    out_descriptor->segment = (uint16_t)(bdf >> 16u);
    out_descriptor->bus = (uint8_t)(bdf >> 8u);
    out_descriptor->device = (uint8_t)((bdf >> 3u) & 0x1fu);
    out_descriptor->function = (uint8_t)(bdf & 0x07u);
    out_descriptor->capabilities =
        (uint8_t)(encoded >> NETD_UEVENT_INPUT_CAPABILITIES_SHIFT);
    out_descriptor->event_index =
        (uint16_t)(encoded >> NETD_UEVENT_INPUT_EVENT_INDEX_SHIFT);
    return 1;
}

typedef struct netd_socket {
    uint64_t domain;
    uint64_t type;
    uint64_t protocol;
    uint64_t flags;
    uint64_t reserved[4];
} netd_socket_t;

typedef struct netd_socket_pair {
    uint64_t domain;
    uint64_t type;
    uint64_t protocol;
    uint64_t handles[2];
} netd_socket_pair_t;

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

typedef struct netd_unix_path {
    uint64_t handle;
    uint64_t flags;
    int32_t pid;
    uint32_t uid;
    uint32_t gid;
    uint32_t reserved0;
    char path[108];
} netd_unix_path_t;

typedef struct netd_listen {
    uint64_t handle;
    int32_t backlog;
    uint32_t reserved0;
} netd_listen_t;

typedef struct netd_netlink_bind {
    uint64_t handle;
    uint32_t pid;
    uint32_t groups;
} netd_netlink_bind_t;

typedef struct netd_accept {
    uint64_t handle;
    uint64_t accepted_handle;
    int32_t pid;
    uint32_t uid;
    uint32_t gid;
    uint32_t notify_ack;
} netd_accept_t;

typedef struct netd_transfer_occurrence {
    uint32_t provider_id;
    uint16_t capability_first;
    uint16_t capability_count;
    uint64_t transfer_token;
    uint64_t rights;
    uint32_t fd_flags;
    uint32_t reserved0;
} netd_transfer_occurrence_t;

typedef struct netd_io {
    uint64_t handle;
    uint64_t length;
    uint64_t flags;
    netd_sockaddr_in_t addr;
    uint64_t transaction_id;
    uint32_t transfer_count;
    uint32_t capability_count;
    /* Readiness bits obtained from this socket's native notification
     * channel.  This acknowledges exactly the coalesced edges consumed by
     * the caller without requiring a separate NETD_OP_POLL round trip. */
    uint64_t notify_ack;
    netd_transfer_occurrence_t transfers[NETD_TRANSFER_MAX_ITEMS];
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

_Static_assert(sizeof(netd_transfer_occurrence_t) == 32,
    "netd transfer occurrence size");
_Static_assert(sizeof(netd_io_t) <= NETD_PAGE_BYTES,
    "netd io page layout");
