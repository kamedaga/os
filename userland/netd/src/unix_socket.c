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
    uint32_t fd_offset;
    uint32_t fd_kind;
    uint32_t fd_flags;
    uint64_t fd_handle;
    uint64_t fd_aux;
    int fd_wait_fd;
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

static void reap_orphaned_sockets(void) {
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
    reap_orphaned_sockets();
    netd_unix_socket_state_t *s = alloc_socket();
    if (s == NULL) return -24;
    s->type = (uint32_t)type;
    s->notify_fd = notify_fd;
    s->fd_wait_fd = -1;
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
    server->fd_wait_fd = -1;
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

int netd_unix_socket_send(const netd_io_t *req, int passed_wait_fd, size_t *out_sent) {
    netd_unix_socket_state_t *s = req ? find_socket(req->handle) : NULL;
    netd_unix_socket_state_t *peer = s ? find_socket(s->peer) : NULL;
    if (s == NULL || peer == NULL || !s->connected) return -107;
    if (req->length > NETD_UNIX_RX - peer->rx_len) return -11;
    if (req->fd_kind != 0 && peer->fd_kind != 0) return -11;
    if (req->fd_kind != 0 && req->length == 0) return -22;
    if (req->fd_kind != 0 &&
        req->fd_kind != NETD_FD_KIND_FILED_MEMFD && passed_wait_fd < 16) return -22;
    if (req->fd_kind == NETD_FD_KIND_FILED_MEMFD && passed_wait_fd >= 16) return -22;
    const uint32_t fd_offset = peer->rx_len;
    memcpy(peer->rx + peer->rx_len, req->data, (size_t)req->length);
    peer->rx_len += (uint32_t)req->length;
    if (req->fd_kind != 0) {
        peer->fd_offset = fd_offset;
        peer->fd_kind = req->fd_kind; peer->fd_flags = req->fd_flags;
        peer->fd_handle = req->fd_handle; peer->fd_aux = req->fd_aux;
        peer->fd_wait_fd = passed_wait_fd;
    }
    notify_readable(peer);
    *out_sent = (size_t)req->length;
    return 0;
}

int netd_unix_socket_recv(netd_io_t *req, size_t capacity, int *out_wait_fd, size_t *out_received) {
    if (req != NULL) {
        req->fd_kind = 0; req->fd_flags = 0;
        req->fd_handle = 0; req->fd_aux = 0;
    }
    if (out_wait_fd != NULL) *out_wait_fd = -1;
    netd_unix_socket_state_t *s = req ? find_socket(req->handle) : NULL;
    if (s == NULL) return -9;
    if (s->rx_len == 0) return s->peer_closed ? 0 : -11;
    size_t n = capacity < s->rx_len ? capacity : s->rx_len;
    if ((req->flags & NETD_MSG_PEEK) != 0) {
        memcpy(req->data, s->rx, n);
        *out_received = n;
        return 0;
    }
    const int deliver_fd = s->fd_kind != 0 && s->fd_offset < n;
    memcpy(req->data, s->rx, n);
    memmove(s->rx, s->rx + n, s->rx_len - n);
    s->rx_len -= (uint32_t)n;
    if (deliver_fd) {
        req->fd_kind = s->fd_kind; req->fd_flags = s->fd_flags;
        req->fd_handle = s->fd_handle; req->fd_aux = s->fd_aux;
        if (out_wait_fd != NULL) {
            *out_wait_fd = s->fd_wait_fd;
        } else if (s->fd_wait_fd >= 16) {
            (void)pacha_fd_close(s->fd_wait_fd);
        }
        s->fd_offset = 0; s->fd_kind = 0; s->fd_flags = 0;
        s->fd_handle = 0; s->fd_aux = 0; s->fd_wait_fd = -1;
    } else if (s->fd_kind != 0) {
        s->fd_offset -= (uint32_t)n;
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
    if (s->fd_wait_fd >= 16) (void)pacha_fd_close(s->fd_wait_fd);
    if (s->fd_kind == NETD_FD_KIND_FILED_MEMFD && s->fd_handle != 0)
        (void)netd_filed_close_handle(s->fd_handle);
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
