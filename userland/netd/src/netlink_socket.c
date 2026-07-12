#include "netlink_socket.h"

#include <string.h>

#define NETD_NETLINK_HANDLE_BIT (1ull << 62)
#define NETD_NETLINK_MAX 16u

typedef struct netd_netlink_socket_state {
    uint64_t handle;
    uint32_t pid;
    uint32_t groups;
    uint8_t bound;
} netd_netlink_socket_state_t;

static netd_netlink_socket_state_t sockets[NETD_NETLINK_MAX];
static uint64_t next_handle;

int netd_netlink_socket_is_handle(uint64_t handle)
{
    return (handle & NETD_NETLINK_HANDLE_BIT) != 0 && (handle >> 63) == 0;
}

static netd_netlink_socket_state_t *find_socket(uint64_t handle)
{
    for (unsigned i = 0; i < NETD_NETLINK_MAX; i++)
        if (sockets[i].handle == handle) return &sockets[i];
    return NULL;
}

int netd_netlink_socket_open(uint64_t type, uint64_t protocol, uint64_t *out_handle)
{
    if (out_handle == NULL ||
        (type != NETD_SOCK_DGRAM && type != NETD_SOCK_RAW) ||
        protocol != NETD_NETLINK_KOBJECT_UEVENT) return -93;
    for (unsigned i = 0; i < NETD_NETLINK_MAX; i++) {
        if (sockets[i].handle != 0) continue;
        memset(&sockets[i], 0, sizeof(sockets[i]));
        sockets[i].handle = NETD_NETLINK_HANDLE_BIT | ++next_handle;
        *out_handle = sockets[i].handle;
        return 0;
    }
    return -24;
}

int netd_netlink_socket_bind(const netd_netlink_bind_t *request)
{
    netd_netlink_socket_state_t *socket = request ? find_socket(request->handle) : NULL;
    if (socket == NULL) return -9;
    socket->pid = request->pid;
    socket->groups = request->groups;
    socket->bound = 1;
    return 0;
}

int netd_netlink_socket_recv(netd_io_t *request, size_t capacity, size_t *out_received)
{
    (void)capacity;
    if (request == NULL || out_received == NULL || find_socket(request->handle) == NULL) return -9;
    *out_received = 0;
    return -11;
}

int netd_netlink_socket_poll(uint64_t handle, uint32_t events, uint32_t *out_revents, int32_t *out_error)
{
    (void)events;
    if (find_socket(handle) == NULL || out_revents == NULL || out_error == NULL) return -9;
    *out_revents = 0;
    *out_error = 0;
    return 0;
}

int netd_netlink_socket_close(uint64_t handle)
{
    netd_netlink_socket_state_t *socket = find_socket(handle);
    if (socket == NULL) return -9;
    memset(socket, 0, sizeof(*socket));
    return 0;
}
