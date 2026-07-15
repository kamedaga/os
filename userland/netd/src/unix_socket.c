#include "unix_socket.h"
#include "libuinet_backend.h"
#include "pacha/ipc.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define NETD_UNIX_HANDLE_BIT (1ull << 63)
#define NETD_UNIX_MAX 32u
#define NETD_UNIX_RX 16384u
#define NETD_MSG_PEEK 0x0002u

typedef struct netd_unix_socket_state {
    uint64_t handle;
    uint64_t peer;
    uint64_t pending;
    uint32_t type;
    uint8_t listening;
    uint8_t connected;
    uint8_t peer_closed;
    uint8_t reserved0;
    int32_t pid;
    uint32_t uid;
    uint32_t gid;
    uint32_t rx_len;
    uint32_t refs;
    uint32_t transfer_offset;
    uint32_t transfer_count;
    uint32_t capability_count;
    uint32_t reserved1;
    uint64_t transaction_id;
    netd_transfer_occurrence_t transfers[NETD_TRANSFER_MAX_ITEMS];
    int capability_fds[NETD_TRANSFER_MAX_CAPABILITIES];
    int notify_fd;
    uint8_t notify_pending;
    char path[108];
    uint8_t rx[NETD_UNIX_RX];
} netd_unix_socket_state_t;

static netd_unix_socket_state_t sockets[NETD_UNIX_MAX];
static uint64_t next_handle;
static void force_destroy_socket(netd_unix_socket_state_t *s);

static void notify_readable(netd_unix_socket_state_t *s) {
    if (s == NULL || s->notify_fd < 16 || s->notify_pending) return;
    const struct pacha_ipc_msg message = { .word0 = 1 };
    if (pacha_ipc_send(s->notify_fd, &message) == 0) s->notify_pending = 1;
}

int netd_unix_socket_is_handle(uint64_t handle) { return (handle & NETD_UNIX_HANDLE_BIT) != 0; }

static netd_unix_socket_state_t *find_socket(uint64_t handle) {
    for (unsigned i = 0; i < NETD_UNIX_MAX; i++) if (sockets[i].handle == handle) return &sockets[i];
    return NULL;
}

int netd_unix_socket_collect_wait_sources(struct pacha_service_wait_set *wait_set) {
    if (wait_set == NULL) return -22;
    for (unsigned i = 0; i < NETD_UNIX_MAX; i++) {
        const netd_unix_socket_state_t *s = &sockets[i];
        if (s->handle == 0 || s->notify_fd < 16) continue;
        if (pacha_service_wait_add(
                wait_set, s->notify_fd, PACHA_FD_EVENT_HANGUP) != 0)
            return -24;
    }
    return 0;
}

void netd_unix_socket_reap_hangups(void) {
    for (unsigned i = 0; i < NETD_UNIX_MAX; i++) {
        netd_unix_socket_state_t *s = &sockets[i];
        if (s->handle == 0 || s->notify_fd < 16) continue;
        struct pacha_pollfd pollfd = {
            .fd = s->notify_fd,
            .events = PACHA_FD_EVENT_HANGUP,
        };
        if (pacha_fd_poll(&pollfd, 1) <= 0 ||
            (pollfd.revents & PACHA_FD_EVENT_HANGUP) == 0) continue;
        const uint64_t handle = s->handle;
        force_destroy_socket(s);
        printf("[netd] unix_orphan_reap handle=%llu\n",
               (unsigned long long)handle);
    }
}

static netd_unix_socket_state_t *alloc_socket(void) {
    for (unsigned i = 0; i < NETD_UNIX_MAX; i++) if (sockets[i].handle == 0) {
        memset(&sockets[i], 0, sizeof(sockets[i]));
        sockets[i].handle = NETD_UNIX_HANDLE_BIT | ++next_handle;
        sockets[i].refs = 1;
        return &sockets[i];
    }
    return NULL;
}

int netd_unix_socket_open(uint64_t type, uint64_t protocol, int notify_fd, uint64_t *out_handle) {
    if (out_handle == NULL || type != NETD_SOCK_STREAM || protocol != 0 || notify_fd < 16) return -94;
    netd_unix_socket_state_t *s = alloc_socket();
    if (s == NULL) return -24;
    s->type = (uint32_t)type;
    s->notify_fd = notify_fd;
    *out_handle = s->handle;
    return 0;
}

int netd_unix_socket_dup(uint64_t handle) {
    netd_unix_socket_state_t *s = find_socket(handle);
    if (s == NULL) return -9;
    if (s->refs == UINT32_MAX) return -24;
    s->refs++;
    return 0;
}

int netd_unix_socket_attach_wait(uint64_t handle, int notify_fd) {
    netd_unix_socket_state_t *s = find_socket(handle);
    if (s == NULL || notify_fd < 16 || s->notify_fd >= 16) return -22;
    s->notify_fd = notify_fd;
    if (s->rx_len != 0 || s->peer_closed || (s->listening && s->pending != 0))
        notify_readable(s);
    return 0;
}

int netd_unix_socket_bind(const netd_unix_path_t *req) {
    netd_unix_socket_state_t *s = req ? find_socket(req->handle) : NULL;
    if (s == NULL || req->path[0] == 0) return -22;
    for (unsigned i = 0; i < NETD_UNIX_MAX; i++) {
        if (sockets[i].handle == 0 || sockets[i].path[0] == 0 ||
            strcmp(sockets[i].path, req->path) != 0) continue;
        force_destroy_socket(&sockets[i]);
    }
    memcpy(s->path, req->path, sizeof(s->path));
    s->path[sizeof(s->path) - 1] = 0;
    s->pid = req->pid; s->uid = req->uid; s->gid = req->gid;
    return 0;
}

int netd_unix_socket_listen(uint64_t handle) {
    netd_unix_socket_state_t *s = find_socket(handle);
    if (s == NULL || s->path[0] == 0) return -22;
    s->listening = 1;
    return 0;
}

int netd_unix_socket_connect(const netd_unix_path_t *req) {
    netd_unix_socket_state_t *client = req ? find_socket(req->handle) : NULL;
    if (client == NULL) return -9;
    netd_unix_socket_state_t *listener = NULL;
    for (unsigned i = 0; i < NETD_UNIX_MAX; i++)
        if (sockets[i].listening && strcmp(sockets[i].path, req->path) == 0) { listener = &sockets[i]; break; }
    if (listener == NULL) return -111;
    if (listener->pending != 0) return -11;
    netd_unix_socket_state_t *server = alloc_socket();
    if (server == NULL) return -24;
    server->type = client->type; server->connected = 1; server->peer = client->handle;
    server->pid = listener->pid; server->uid = listener->uid; server->gid = listener->gid;
    client->connected = 1; client->peer = server->handle;
    client->pid = req->pid; client->uid = req->uid; client->gid = req->gid;
    listener->pending = server->handle;
    server->notify_fd = -1;
    notify_readable(listener);
    return 0;
}

int netd_unix_socket_accept(netd_accept_t *req) {
    netd_unix_socket_state_t *listener = req ? find_socket(req->handle) : NULL;
    if (listener == NULL || !listener->listening) return -22;
    if (listener->pending == 0) return -11;
    netd_unix_socket_state_t *server = find_socket(listener->pending);
    netd_unix_socket_state_t *client = server ? find_socket(server->peer) : NULL;
    if (server == NULL || client == NULL) return -5;
    req->accepted_handle = server->handle;
    req->pid = client->pid; req->uid = client->uid; req->gid = client->gid;
    listener->pending = 0;
    listener->notify_pending = 0;
    return 0;
}

static int netd_unix_transfer_valid(
    const netd_io_t *req,
    const int *capability_fds,
    uint32_t capability_count)
{
    if (req->transfer_count > NETD_TRANSFER_MAX_ITEMS ||
        req->capability_count > NETD_TRANSFER_MAX_CAPABILITIES ||
        req->capability_count != capability_count)
        return 0;
    if (req->transfer_count == 0)
        return req->transaction_id == 0 && capability_count == 0 &&
            req->reserved0 == 0;
    if (req->transaction_id == 0 || req->length == 0 ||
        req->reserved0 != 0 || capability_fds == NULL)
        return 0;
    for (uint32_t i = 0; i < capability_count; ++i)
        if (capability_fds[i] < 16) return 0;
    uint32_t next_capability = 0;
    for (uint32_t i = 0; i < req->transfer_count; ++i) {
        const netd_transfer_occurrence_t *item = &req->transfers[i];
        const uint32_t end = (uint32_t)item->capability_first + item->capability_count;
        if (item->provider_id == 0 || item->transfer_token == 0 ||
            item->reserved0 != 0 || item->capability_count == 0 ||
            item->capability_first != next_capability || end > capability_count)
            return 0;
        next_capability = end;
    }
    return next_capability == capability_count;
}

int netd_unix_socket_send(
    const netd_io_t *req,
    const int *capability_fds,
    uint32_t capability_count,
    size_t *out_sent)
{
    netd_unix_socket_state_t *s = req ? find_socket(req->handle) : NULL;
    netd_unix_socket_state_t *peer = s ? find_socket(s->peer) : NULL;
    if (s == NULL || peer == NULL || !s->connected) return -107;
    if (req->length > NETD_UNIX_RX - peer->rx_len) return -11;
    if (req->transfer_count != 0 && peer->transfer_count != 0) return -11;
    if (!netd_unix_transfer_valid(req, capability_fds, capability_count)) return -22;
    const uint32_t transfer_offset = peer->rx_len;
    memcpy(peer->rx + peer->rx_len, req->data, (size_t)req->length);
    peer->rx_len += (uint32_t)req->length;
    if (req->transfer_count != 0) {
        peer->transfer_offset = transfer_offset;
        peer->transaction_id = req->transaction_id;
        peer->transfer_count = req->transfer_count;
        peer->capability_count = capability_count;
        memcpy(peer->transfers, req->transfers,
            sizeof(req->transfers[0]) * req->transfer_count);
        for (uint32_t i = 0; i < capability_count; ++i)
            peer->capability_fds[i] = capability_fds[i];
    }
    notify_readable(peer);
    *out_sent = (size_t)req->length;
    return 0;
}

int netd_unix_socket_recv(
    netd_io_t *req,
    size_t capacity,
    int *out_capability_fds,
    uint32_t capability_capacity,
    uint32_t *out_capability_count,
    size_t *out_received)
{
    if (req != NULL) {
        req->transaction_id = 0;
        req->transfer_count = 0;
        req->capability_count = 0;
        req->reserved0 = 0;
        memset(req->transfers, 0, sizeof(req->transfers));
    }
    if (out_capability_count != NULL) *out_capability_count = 0;
    netd_unix_socket_state_t *s = req ? find_socket(req->handle) : NULL;
    if (s == NULL) return -9;
    if (s->rx_len == 0) return s->peer_closed ? 0 : -11;
    size_t n = capacity < s->rx_len ? capacity : s->rx_len;
    if ((req->flags & NETD_MSG_PEEK) != 0) {
        memcpy(req->data, s->rx, n);
        *out_received = n;
        return 0;
    }
    const int deliver_transfer = s->transfer_count != 0 && s->transfer_offset < n;
    if (deliver_transfer && (out_capability_fds == NULL ||
        out_capability_count == NULL || capability_capacity < s->capability_count))
        return -90;
    memcpy(req->data, s->rx, n);
    memmove(s->rx, s->rx + n, s->rx_len - n);
    s->rx_len -= (uint32_t)n;
    if (deliver_transfer) {
        req->transaction_id = s->transaction_id;
        req->transfer_count = s->transfer_count;
        req->capability_count = s->capability_count;
        memcpy(req->transfers, s->transfers,
            sizeof(req->transfers[0]) * s->transfer_count);
        for (uint32_t i = 0; i < s->capability_count; ++i) {
            out_capability_fds[i] = s->capability_fds[i];
            s->capability_fds[i] = -1;
        }
        *out_capability_count = s->capability_count;
        s->transfer_offset = 0;
        s->transaction_id = 0;
        s->transfer_count = 0;
        s->capability_count = 0;
        memset(s->transfers, 0, sizeof(s->transfers));
    } else if (s->transfer_count != 0) {
        s->transfer_offset -= (uint32_t)n;
    }
    s->notify_pending = 0;
    if (s->rx_len != 0) notify_readable(s);
    *out_received = n;
    return 0;
}

int netd_unix_socket_poll(uint64_t handle, uint32_t events, uint32_t *out_revents, int32_t *out_error) {
    netd_unix_socket_state_t *s = find_socket(handle);
    if (s == NULL) return -9;
    *out_error = 0; *out_revents = 0;
    if ((events & NETD_POLLIN) && ((s->listening && s->pending != 0) || s->rx_len != 0 || s->peer_closed)) *out_revents |= NETD_POLLIN;
    if ((events & NETD_POLLOUT) && s->connected) *out_revents |= NETD_POLLOUT;
    return 0;
}

static void force_destroy_socket(netd_unix_socket_state_t *s) {
    if (s == NULL || s->handle == 0) return;
    const uint64_t pending_handle = s->pending;
    s->pending = 0;
    if (pending_handle != 0) {
        netd_unix_socket_state_t *pending = find_socket(pending_handle);
        if (pending != NULL && pending != s) force_destroy_socket(pending);
    }
    netd_unix_socket_state_t *peer = find_socket(s->peer);
    if (peer) { peer->peer_closed = 1; peer->peer = 0; notify_readable(peer); }
    for (uint32_t i = 0; i < s->capability_count; ++i)
        if (s->capability_fds[i] >= 16) (void)pacha_fd_close(s->capability_fds[i]);
    if (s->notify_fd >= 16) (void)pacha_fd_close(s->notify_fd);
    memset(s, 0, sizeof(*s));
}

int netd_unix_socket_close(uint64_t handle) {
    netd_unix_socket_state_t *s = find_socket(handle);
    if (s == NULL) return -9;
    if (s->refs > 1) {
        s->refs--;
        return 0;
    }
    char path[sizeof(s->path)];
    memcpy(path, s->path, sizeof(path));
    path[sizeof(path) - 1] = 0;
    force_destroy_socket(s);
    unsigned active = 0;
    unsigned refs = 0;
    for (unsigned i = 0; i < NETD_UNIX_MAX; i++) {
        if (sockets[i].handle == 0) continue;
        active++;
        refs += sockets[i].refs;
    }
    printf("[netd] unix_close path=%s active=%u refs=%u\n",
           path[0] != 0 ? path : "-", active, refs);
    fflush(stdout);
    return 0;
}
