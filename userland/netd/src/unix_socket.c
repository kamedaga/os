#include "unix_socket.h"
#include "libuinet_backend.h"
#include "pacha/ipc.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define NETD_UNIX_HANDLE_BIT (1ull << 63)
#define NETD_UNIX_MAX 32u
#define NETD_UNIX_RX NETD_IO_BYTES
#define NETD_UNIX_PACKET_MAX 256u
#define NETD_MSG_PEEK 0x0002u
#define NETD_MSG_TRUNC 0x0020u

typedef struct netd_unix_socket_state {
    uint64_t handle;
    uint64_t peer;
    uint64_t pending_head;
    uint64_t pending_tail;
    uint64_t pending_next;
    uint32_t type;
    uint32_t backlog;
    uint32_t pending_count;
    uint8_t listening;
    uint8_t connected;
    uint8_t peer_closed;
    uint8_t reserved0;
    int32_t pid;
    uint32_t uid;
    uint32_t gid;
    uint32_t rx_head;
    uint32_t rx_len;
    uint32_t refs;
    uint32_t transfer_offset;
    uint32_t transfer_count;
    uint32_t capability_count;
    uint16_t packet_head;
    uint16_t packet_count;
    uint16_t transfer_packet_offset;
    uint16_t reserved1;
    uint64_t transaction_id;
    netd_transfer_occurrence_t transfers[NETD_TRANSFER_MAX_ITEMS];
    int capability_fds[NETD_TRANSFER_MAX_CAPABILITIES];
    int notify_fd;
    uint32_t notify_pending;
    char path[108];
    uint32_t packet_lengths[NETD_UNIX_PACKET_MAX];
    uint8_t rx[NETD_UNIX_RX];
} netd_unix_socket_state_t;

static netd_unix_socket_state_t sockets[NETD_UNIX_MAX];
static uint64_t next_handle;
static void force_destroy_socket(netd_unix_socket_state_t *s);
static int socket_has_data(const netd_unix_socket_state_t *s);

static uint32_t readable_events(const netd_unix_socket_state_t *s)
{
    if (s == NULL) return 0;
    uint32_t events = 0;
    if ((s->listening && s->pending_head != 0) ||
        socket_has_data(s) || s->peer_closed)
        events |= NETD_POLLIN;
    if (s->peer_closed) events |= NETD_POLLHUP;
    return events;
}

static void notify_events(netd_unix_socket_state_t *s, uint32_t events) {
    if (s == NULL || s->notify_fd < 16) return;
    events &= NETD_POLLIN | NETD_POLLOUT | NETD_POLLHUP;
    const uint32_t fresh = events & ~s->notify_pending;
    if (fresh == 0) return;
    const struct pacha_ipc_msg message = { .word0 = fresh };
    const int status = pacha_ipc_send(s->notify_fd, &message);
    if (status == 0) s->notify_pending |= fresh;
}

int netd_unix_socket_is_handle(uint64_t handle) { return (handle & NETD_UNIX_HANDLE_BIT) != 0; }

static netd_unix_socket_state_t *find_socket(uint64_t handle) {
    for (unsigned i = 0; i < NETD_UNIX_MAX; i++) if (sockets[i].handle == handle) return &sockets[i];
    return NULL;
}

static int socket_has_data(const netd_unix_socket_state_t *s)
{
    return s != NULL && (s->type == NETD_SOCK_SEQPACKET ?
        s->packet_count != 0 : s->rx_len != 0);
}

static void netd_unix_rx_copy_in(
    netd_unix_socket_state_t *s,
    const uint8_t *source,
    uint32_t length)
{
    if (length == 0) return;
    const uint32_t tail = (s->rx_head + s->rx_len) % NETD_UNIX_RX;
    const uint32_t first = length < NETD_UNIX_RX - tail ?
        length : NETD_UNIX_RX - tail;
    memcpy(s->rx + tail, source, first);
    if (length != first) memcpy(s->rx, source + first, length - first);
    s->rx_len += length;
}

static void netd_unix_rx_copy_out(
    const netd_unix_socket_state_t *s,
    uint8_t *destination,
    uint32_t length)
{
    if (length == 0) return;
    const uint32_t first = length < NETD_UNIX_RX - s->rx_head ?
        length : NETD_UNIX_RX - s->rx_head;
    memcpy(destination, s->rx + s->rx_head, first);
    if (length != first) memcpy(destination + first, s->rx, length - first);
}

static void netd_unix_rx_consume(
    netd_unix_socket_state_t *s,
    uint32_t length)
{
    if (length == 0) return;
    s->rx_head = (s->rx_head + length) % NETD_UNIX_RX;
    s->rx_len -= length;
    if (s->rx_len == 0) s->rx_head = 0;
}

static netd_unix_socket_state_t *dequeue_pending(
    netd_unix_socket_state_t *listener)
{
    if (listener == NULL || listener->pending_head == 0) return NULL;
    netd_unix_socket_state_t *server = find_socket(listener->pending_head);
    if (server == NULL) {
        listener->pending_head = 0;
        listener->pending_tail = 0;
        listener->pending_count = 0;
        return NULL;
    }
    listener->pending_head = server->pending_next;
    server->pending_next = 0;
    if (listener->pending_count != 0) listener->pending_count--;
    if (listener->pending_head == 0) listener->pending_tail = 0;
    return server;
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

void netd_unix_socket_reap_hangups(
    const struct pacha_service_wait_set *wait_set) {
    for (unsigned i = 0; i < NETD_UNIX_MAX; i++) {
        netd_unix_socket_state_t *s = &sockets[i];
        if (s->handle == 0 || s->notify_fd < 16) continue;
        if ((pacha_service_wait_revents(wait_set, s->notify_fd) &
             PACHA_FD_EVENT_HANGUP) == 0) continue;
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
    if (out_handle == NULL ||
        (type != NETD_SOCK_STREAM && type != NETD_SOCK_SEQPACKET) ||
        protocol != 0 || notify_fd < 16) return -94;
    netd_unix_socket_state_t *s = alloc_socket();
    if (s == NULL) return -24;
    s->type = (uint32_t)type;
    s->notify_fd = notify_fd;
    *out_handle = s->handle;
    return 0;
}

int netd_unix_socket_pair(
    uint64_t type,
    uint64_t protocol,
    int first_notify_fd,
    int second_notify_fd,
    uint64_t out_handles[2])
{
    if (out_handles == NULL ||
        (type != NETD_SOCK_STREAM && type != NETD_SOCK_SEQPACKET) ||
        protocol != 0 ||
        first_notify_fd < 16 || second_notify_fd < 16) return -94;
    netd_unix_socket_state_t *first = alloc_socket();
    if (first == NULL) return -24;
    netd_unix_socket_state_t *second = alloc_socket();
    if (second == NULL) {
        memset(first, 0, sizeof(*first));
        return -24;
    }
    first->type = (uint32_t)type;
    first->connected = 1;
    first->peer = second->handle;
    first->notify_fd = first_notify_fd;
    second->type = (uint32_t)type;
    second->connected = 1;
    second->peer = first->handle;
    second->notify_fd = second_notify_fd;
    out_handles[0] = first->handle;
    out_handles[1] = second->handle;
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
    if (socket_has_data(s) || s->peer_closed ||
        (s->listening && s->pending_head != 0))
        notify_events(s, readable_events(s));
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

int netd_unix_socket_listen(const netd_listen_t *req) {
    netd_unix_socket_state_t *s = req ? find_socket(req->handle) : NULL;
    if (req == NULL || s == NULL || s->path[0] == 0 || req->reserved0 != 0) {
        return -22;
    }
    uint32_t backlog = req->backlog > 0 ? (uint32_t)req->backlog : 1u;
    if (backlog >= NETD_UNIX_MAX) backlog = NETD_UNIX_MAX - 1u;
    s->backlog = backlog;
    s->listening = 1;
    return 0;
}

int netd_unix_socket_connect(const netd_unix_path_t *req) {
    netd_unix_socket_state_t *client = req ? find_socket(req->handle) : NULL;
    if (client == NULL) return -9;
    netd_unix_socket_state_t *listener = NULL;
    for (unsigned i = 0; i < NETD_UNIX_MAX; i++)
        if (sockets[i].listening && strcmp(sockets[i].path, req->path) == 0) {
            listener = &sockets[i];
            break;
        }
    if (listener == NULL) return -111;
    if (listener->type != client->type) return -91;
    if (listener->pending_count >= listener->backlog) return -11;
    netd_unix_socket_state_t *server = alloc_socket();
    if (server == NULL) return -24;
    server->type = client->type; server->connected = 1; server->peer = client->handle;
    server->pid = req->pid; server->uid = req->uid; server->gid = req->gid;
    client->connected = 1; client->peer = server->handle;
    client->pid = req->pid; client->uid = req->uid; client->gid = req->gid;
    if (listener->pending_tail != 0) {
        netd_unix_socket_state_t *tail = find_socket(listener->pending_tail);
        if (tail == NULL) {
            force_destroy_socket(server);
            return -5;
        }
        tail->pending_next = server->handle;
    } else {
        listener->pending_head = server->handle;
    }
    listener->pending_tail = server->handle;
    listener->pending_count++;
    server->notify_fd = -1;
    notify_events(listener, NETD_POLLIN);
    return 0;
}

int netd_unix_socket_accept(netd_accept_t *req) {
    netd_unix_socket_state_t *listener = req ? find_socket(req->handle) : NULL;
    if (listener == NULL || !listener->listening ||
        (req->notify_ack &
         ~(uint32_t)(NETD_POLLIN | NETD_POLLOUT | NETD_POLLHUP)) != 0)
        return -22;
    /* The caller drains the listener doorbell before ACCEPT.  A stale edge
     * may race with another acceptor, so acknowledge it even when the FIFO is
     * already empty and let the next empty -> pending transition notify. */
    listener->notify_pending &= ~req->notify_ack;
    netd_unix_socket_state_t *server = dequeue_pending(listener);
    if (server == NULL) return -11;
    req->accepted_handle = server->handle;
    req->pid = server->pid; req->uid = server->uid; req->gid = server->gid;
    if (listener->pending_head != 0)
        notify_events(listener, NETD_POLLIN);
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
    if ((req->notify_ack &
         ~(uint64_t)(NETD_POLLIN | NETD_POLLOUT | NETD_POLLHUP)) != 0)
        return 0;
    if (req->transfer_count == 0)
        return req->transaction_id == 0 && capability_count == 0;
    if (req->transaction_id == 0 ||
        capability_fds == NULL)
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
    if (s == NULL || peer == NULL || !s->connected || out_sent == NULL)
        return -107;
    if (!netd_unix_transfer_valid(req, capability_fds, capability_count))
        return -22;
    s->notify_pending &= ~(uint32_t)req->notify_ack;

    const size_t available = NETD_UNIX_RX - peer->rx_len;
    if (req->transfer_count != 0 && peer->transfer_count != 0) return -11;

    if (s->type == NETD_SOCK_SEQPACKET) {
        if (req->length > NETD_UNIX_RX) return -90;
        if (peer->packet_count == NETD_UNIX_PACKET_MAX ||
            req->length > available || peer->transfer_count != 0)
            return -11;
        const uint32_t packet_slot =
            ((uint32_t)peer->packet_head + peer->packet_count) %
            NETD_UNIX_PACKET_MAX;
        const uint32_t transfer_offset = peer->rx_len;
        if (req->length != 0) {
            netd_unix_rx_copy_in(peer, req->data, (uint32_t)req->length);
        }
        if (req->transfer_count != 0) {
            peer->transfer_offset = transfer_offset;
            peer->transfer_packet_offset = peer->packet_count;
            peer->transaction_id = req->transaction_id;
            peer->transfer_count = req->transfer_count;
            peer->capability_count = capability_count;
            memcpy(peer->transfers, req->transfers,
                sizeof(req->transfers[0]) * req->transfer_count);
            for (uint32_t i = 0; i < capability_count; ++i)
                peer->capability_fds[i] = capability_fds[i];
        }
        peer->packet_lengths[packet_slot] = (uint32_t)req->length;
        peer->packet_count++;
        notify_events(peer, NETD_POLLIN);
        *out_sent = (size_t)req->length;
        return 0;
    }

    if (available == 0) return -11;
    if (req->transfer_count != 0 && req->length == 0) return -22;
    if (req->length == 0) {
        *out_sent = 0;
        return 0;
    }
    const size_t sent = (size_t)req->length < available ?
        (size_t)req->length : available;
    const uint32_t transfer_offset = peer->rx_len;
    netd_unix_rx_copy_in(peer, req->data, (uint32_t)sent);
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
    notify_events(peer, NETD_POLLIN);
    *out_sent = sent;
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
    uint64_t input_flags = 0;
    uint64_t notify_ack = 0;
    if (req != NULL) {
        input_flags = req->flags;
        notify_ack = req->notify_ack;
        req->flags = 0;
        req->transaction_id = 0;
        req->transfer_count = 0;
        req->capability_count = 0;
        req->notify_ack = 0;
        memset(req->transfers, 0, sizeof(req->transfers));
    }
    if (out_capability_count != NULL) *out_capability_count = 0;
    netd_unix_socket_state_t *s = req ? find_socket(req->handle) : NULL;
    if (s == NULL) return -9;
    /* The caller drains the native doorbell before issuing RECV.  A stale
     * readable observation may therefore reach us after another reader has
     * consumed the bytes.  Acknowledge the drained doorbell even on EAGAIN so
     * the next empty -> readable transition can notify again. */
    if ((notify_ack &
         ~(uint64_t)(NETD_POLLIN | NETD_POLLOUT | NETD_POLLHUP)) != 0)
        return -22;
    s->notify_pending &= ~(uint32_t)notify_ack;
    if (!socket_has_data(s)) {
        const int status = s->peer_closed ? 0 : -11;
        return status;
    }
    const uint32_t queued = s->type == NETD_SOCK_SEQPACKET ?
        s->packet_lengths[s->packet_head] : s->rx_len;
    size_t n = capacity < queued ? capacity : queued;
    if ((input_flags & NETD_MSG_PEEK) != 0) {
        netd_unix_rx_copy_out(s, req->data, (uint32_t)n);
        if (n < queued && s->type == NETD_SOCK_SEQPACKET)
            req->flags |= NETD_MSG_TRUNC;
        notify_events(s, readable_events(s));
        *out_received = n;
        return 0;
    }
    const int deliver_transfer = s->transfer_count != 0 &&
        (s->type == NETD_SOCK_SEQPACKET ?
         s->transfer_packet_offset == 0 : s->transfer_offset < n);
    if (deliver_transfer && (out_capability_fds == NULL ||
        out_capability_count == NULL || capability_capacity < s->capability_count))
        return -90;
    const int was_full = s->rx_len == NETD_UNIX_RX ||
        s->packet_count == NETD_UNIX_PACKET_MAX;
    netd_unix_rx_copy_out(s, req->data, (uint32_t)n);
    const uint32_t consumed = s->type == NETD_SOCK_SEQPACKET ? queued : (uint32_t)n;
    netd_unix_rx_consume(s, consumed);
    if (s->type == NETD_SOCK_SEQPACKET) {
        if (n < queued) req->flags |= NETD_MSG_TRUNC;
        s->packet_lengths[s->packet_head] = 0;
        s->packet_head = (uint16_t)(
            ((uint32_t)s->packet_head + 1u) % NETD_UNIX_PACKET_MAX);
        s->packet_count--;
    }
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
        s->transfer_packet_offset = 0;
        s->transaction_id = 0;
        s->transfer_count = 0;
        s->capability_count = 0;
        memset(s->transfers, 0, sizeof(s->transfers));
    } else if (s->transfer_count != 0) {
        if (s->type == NETD_SOCK_SEQPACKET) {
            s->transfer_packet_offset--;
            s->transfer_offset -= consumed;
        } else {
            s->transfer_offset -= consumed;
        }
    }
    if (socket_has_data(s) || s->peer_closed)
        notify_events(s, readable_events(s));
    if (was_full) notify_events(find_socket(s->peer), NETD_POLLOUT);
    if (deliver_transfer)
        notify_events(find_socket(s->peer), NETD_POLLOUT);
    *out_received = n;
    return 0;
}

int netd_unix_socket_poll(uint64_t handle, uint32_t events, uint32_t *out_revents, int32_t *out_error) {
    netd_unix_socket_state_t *s = find_socket(handle);
    if (s == NULL) return -9;
    /* POLL is the explicit acknowledgement for the coalesced state-change
     * doorbell.  SEND must not acknowledge it: Wayland traffic is
     * bidirectional, and doing so can enqueue duplicate doorbells until the
     * fixed-size IPC queue fills and a later readiness edge is lost. */
    s->notify_pending = 0;
    *out_error = 0; *out_revents = 0;
    if ((events & NETD_POLLIN) &&
        ((s->listening && s->pending_head != 0) ||
         socket_has_data(s) || s->peer_closed))
        *out_revents |= NETD_POLLIN;
    netd_unix_socket_state_t *peer = find_socket(s->peer);
    if ((events & NETD_POLLOUT) && s->connected && peer != NULL &&
        peer->rx_len < NETD_UNIX_RX &&
        (peer->type != NETD_SOCK_SEQPACKET ||
         peer->packet_count < NETD_UNIX_PACKET_MAX) &&
        peer->transfer_count == 0)
        *out_revents |= NETD_POLLOUT;
    if (s->peer_closed) *out_revents |= NETD_POLLHUP;
    return 0;
}

static void force_destroy_socket(netd_unix_socket_state_t *s) {
    if (s == NULL || s->handle == 0) return;
    while (s->pending_head != 0) {
        netd_unix_socket_state_t *pending = dequeue_pending(s);
        if (pending == NULL) break;
        if (pending != s) force_destroy_socket(pending);
    }
    netd_unix_socket_state_t *peer = find_socket(s->peer);
    if (peer) {
        peer->peer_closed = 1;
        peer->peer = 0;
        notify_events(peer, NETD_POLLIN | NETD_POLLHUP);
    }
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
    if (path[0] == 0) return 0;
    unsigned active = 0;
    unsigned refs = 0;
    for (unsigned i = 0; i < NETD_UNIX_MAX; i++) {
        if (sockets[i].handle == 0) continue;
        active++;
        refs += sockets[i].refs;
    }
    printf("[netd] unix_close path=%s active=%u refs=%u\n",
           path, active, refs);
    fflush(stdout);
    return 0;
}
