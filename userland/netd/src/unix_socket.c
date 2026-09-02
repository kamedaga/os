#include "unix_socket.h"
#include "libuinet_backend.h"
#include "pacha/ipc.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define NETD_UNIX_HANDLE_BIT (1ull << 63)
#define NETD_UNIX_RX NETD_IO_BYTES
#define NETD_UNIX_PACKET_MAX 256u
/* listen(2) backlog is client supplied and every pending connection now
 * allocates a socket plus an RX buffer, so keep it bounded.  1024 matches
 * the wait-set capacity, past which an accepted socket has nowhere to go. */
#define NETD_UNIX_BACKLOG_MAX 1024u
#define NETD_MSG_PEEK 0x0002u
#define NETD_MSG_TRUNC 0x0020u

#ifndef NETD_DBUS_DIAG
#define NETD_DBUS_DIAG 0
#endif

#ifndef NETD_UNIX_DIAG
#define NETD_UNIX_DIAG 0
#endif

#ifndef NETD_X11_DIAG
#define NETD_X11_DIAG 0
#endif

typedef struct netd_unix_socket_state {
    struct netd_unix_socket_state *next;
    struct netd_unix_socket_state *notification_next;
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
    uint8_t diag_dbus_send_count;
    uint8_t diag_dbus_notify_count;
#if NETD_UNIX_DIAG
    uint8_t diag_send_count;
    uint8_t diag_recv_count;
    uint8_t diag_poll_count;
    uint8_t diag_notify_count;
#endif
#if NETD_X11_DIAG
    uint8_t diag_x11_send_count;
#endif
    uint8_t path_abstract;
    uint8_t path_bound;
    uint16_t path_length;
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
    uint32_t notify_deferred;
    uint8_t notify_queued;
    char path[108];
    uint32_t packet_lengths[NETD_UNIX_PACKET_MAX];
    uint8_t *rx;
} netd_unix_socket_state_t;

static netd_unix_socket_state_t *sockets;
static size_t socket_count;
static uint64_t next_handle;
static netd_unix_socket_state_t *notification_head;
static netd_unix_socket_state_t *notification_tail;
static int notifications_deferred;
static void force_destroy_socket(netd_unix_socket_state_t *s);
static netd_unix_socket_state_t *find_socket(uint64_t handle);
static int socket_has_data(const netd_unix_socket_state_t *s);

#if NETD_UNIX_DIAG
static void netd_unix_diag_path(
    const char *op,
    const netd_unix_path_t *req,
    uint64_t listener,
    uint64_t peer,
    int status)
{
    const uint16_t length = req != NULL && req->reserved0 <= sizeof(req->path) ?
        (uint16_t)req->reserved0 : 0;
    printf("[netd-unix-diag] op=%s pid=%d handle=%llu listener=%llu peer=%llu abstract=%u path=%.*s status=%d\n",
        op,
        req != NULL ? req->pid : 0,
        (unsigned long long)(req != NULL ? req->handle : 0),
        (unsigned long long)listener,
        (unsigned long long)peer,
        req != NULL && (req->flags & NETD_UNIX_PATH_ABSTRACT) != 0,
        (int)length,
        req != NULL ? req->path : "",
        status);
}
#endif

#if NETD_DBUS_DIAG
static uint32_t dbus_wire_diag_count;

static uint32_t dbus_diag_u32(const uint8_t *data, int big_endian)
{
    if (big_endian) {
        return ((uint32_t)data[0] << 24u) |
            ((uint32_t)data[1] << 16u) |
            ((uint32_t)data[2] << 8u) |
            data[3];
    }
    return (uint32_t)data[0] |
        ((uint32_t)data[1] << 8u) |
        ((uint32_t)data[2] << 16u) |
        ((uint32_t)data[3] << 24u);
}

static size_t dbus_diag_align(size_t value, size_t alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static void dbus_diag_message(
    const netd_unix_socket_state_t *source,
    const netd_unix_socket_state_t *peer,
    const uint8_t *data,
    size_t length)
{
    if (source == NULL || peer == NULL || data == NULL || length < 16u ||
        (source->reserved0 == 0 && peer->reserved0 == 0) ||
        (data[0] != 'l' && data[0] != 'B') || data[3] != 1u ||
        dbus_wire_diag_count >= 2048u)
    {
        return;
    }
    const int big_endian = data[0] == 'B';
    const uint32_t body_length = dbus_diag_u32(data + 4u, big_endian);
    const uint32_t serial = dbus_diag_u32(data + 8u, big_endian);
    const uint32_t fields_length = dbus_diag_u32(data + 12u, big_endian);
    if (fields_length > length - 16u) return;

    uint32_t reply_serial = 0;
    char member[64];
    member[0] = 0;
    size_t offset = 16u;
    const size_t fields_end = 16u + fields_length;
    while (offset < fields_end) {
        offset = dbus_diag_align(offset, 8u);
        if (offset + 4u > fields_end) break;
        const uint8_t code = data[offset++];
        const uint8_t signature_length = data[offset++];
        if (signature_length == 0u ||
            offset + signature_length + 1u > fields_end)
        {
            break;
        }
        const uint8_t signature = data[offset];
        offset += signature_length + 1u;
        if (signature == 'u') {
            offset = dbus_diag_align(offset, 4u);
            if (offset + 4u > fields_end) break;
            if (code == 5u)
                reply_serial = dbus_diag_u32(data + offset, big_endian);
            offset += 4u;
        } else if (signature == 's' || signature == 'o') {
            offset = dbus_diag_align(offset, 4u);
            if (offset + 4u > fields_end) break;
            const uint32_t string_length =
                dbus_diag_u32(data + offset, big_endian);
            offset += 4u;
            if (string_length > fields_end - offset ||
                offset + string_length + 1u > fields_end)
            {
                break;
            }
            if (code == 3u) {
                const size_t copied = string_length < sizeof(member) - 1u ?
                    string_length : sizeof(member) - 1u;
                memcpy(member, data + offset, copied);
                member[copied] = 0;
            }
            offset += string_length + 1u;
        } else if (signature == 'g') {
            if (offset + 1u > fields_end) break;
            const uint8_t string_length = data[offset++];
            if ((size_t)string_length + 1u > fields_end - offset) break;
            offset += (size_t)string_length + 1u;
        } else {
            break;
        }
    }

    dbus_wire_diag_count++;
    printf(
        "[netd-dbus-wire] count=%u from=%llu to=%llu type=%u "
        "serial=%u reply=%u member=%s bytes=%llu body=%u peer_rx=%u "
        "peer_pending=%u\n",
        dbus_wire_diag_count,
        (unsigned long long)source->handle,
        (unsigned long long)peer->handle,
        data[1],
        serial,
        reply_serial,
        member[0] != 0 ? member : "-",
        (unsigned long long)length,
        body_length,
        peer->rx_len,
        peer->notify_pending);
}
#endif

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

static void notification_enqueue(netd_unix_socket_state_t *s)
{
    if (s == NULL || s->notify_queued) return;
    s->notification_next = NULL;
    if (notification_tail != NULL)
        notification_tail->notification_next = s;
    else
        notification_head = s;
    notification_tail = s;
    s->notify_queued = 1;
}

static void notification_remove(netd_unix_socket_state_t *s)
{
    if (s == NULL || !s->notify_queued) return;
    netd_unix_socket_state_t *previous = NULL;
    netd_unix_socket_state_t *current = notification_head;
    while (current != NULL && current != s) {
        previous = current;
        current = current->notification_next;
    }
    if (current == NULL) {
        s->notification_next = NULL;
        s->notify_queued = 0;
        return;
    }
    if (previous != NULL)
        previous->notification_next = current->notification_next;
    else
        notification_head = current->notification_next;
    if (notification_tail == current) notification_tail = previous;
    current->notification_next = NULL;
    current->notify_queued = 0;
}

static void notify_events(netd_unix_socket_state_t *s, uint32_t events) {
    if (s == NULL || s->notify_fd < 16) return;
    events &= NETD_POLLIN | NETD_POLLOUT | NETD_POLLHUP;
    s->notify_deferred |= events;
    notification_enqueue(s);
    if (notifications_deferred) return;
    (void)netd_unix_socket_flush_notification();
}

void netd_unix_socket_set_notifications_deferred(int deferred)
{
    notifications_deferred = deferred != 0;
}


unsigned netd_unix_socket_flush_notification(void)
{
    netd_unix_socket_state_t *s = notification_head;
    if (s == NULL) return 0;
    notification_head = s->notification_next;
    if (notification_head == NULL) notification_tail = NULL;
    s->notification_next = NULL;
    s->notify_queued = 0;
    if (s->handle == 0 || s->notify_fd < 16) {
        s->notify_deferred = 0;
        return 0;
    }
    const uint32_t fresh = s->notify_deferred & ~s->notify_pending;
    s->notify_deferred = fresh;
    if (fresh == 0) return 0;
    const struct pacha_ipc_msg message = { .word0 = fresh };
    const int status = pacha_ipc_send(s->notify_fd, &message);
    if (status == 0) {
        s->notify_pending |= fresh;
        s->notify_deferred &= ~fresh;
    } else {
        notification_enqueue(s);
    }
#if NETD_UNIX_DIAG
    if (s->diag_notify_count < 8u) {
        s->diag_notify_count++;
        printf("[netd-unix-diag] op=notify handle=%llu peer=%llu events=%u fresh=%u pending=%u rx=%u closed=%u status=%d\n",
            (unsigned long long)s->handle,
            (unsigned long long)s->peer,
            fresh,
            fresh,
            s->notify_pending,
            s->rx_len,
            s->peer_closed,
            status);
    }
#endif
#if NETD_DBUS_DIAG
    if (s->reserved0 != 0 &&
        (status != 0 || s->rx_len >= 512u) &&
        s->diag_dbus_notify_count < 64)
    {
        s->diag_dbus_notify_count++;
        printf("[netd-dbus-wait] op=notify count=%u handle=%llu pid=%d events=%u fresh=%u pending=%u rx=%u status=%d\n",
            (unsigned)s->diag_dbus_notify_count,
            (unsigned long long)s->handle,
            s->pid,
            fresh,
            fresh,
            s->notify_pending,
            s->rx_len,
            status);
    }
#endif
    return status == 0 ? 1u : 0u;
}

int netd_unix_socket_is_handle(uint64_t handle) { return (handle & NETD_UNIX_HANDLE_BIT) != 0; }

int netd_unix_socket_diag_dbus(uint64_t handle)
{
    const netd_unix_socket_state_t *socket = find_socket(handle);
    return socket != NULL && socket->reserved0 != 0;
}

static netd_unix_socket_state_t *find_socket(uint64_t handle) {
    /* Zero is the detached/no-peer sentinel. */
    if (handle == 0) return NULL;
    for (netd_unix_socket_state_t *s = sockets; s != NULL; s = s->next)
        if (s->handle == handle) return s;
    return NULL;
}

static int socket_has_data(const netd_unix_socket_state_t *s)
{
    return s != NULL && (s->type == NETD_SOCK_SEQPACKET ?
        s->packet_count != 0 : s->rx_len != 0);
}

#if NETD_X11_DIAG
static int netd_unix_is_x11_socket(const netd_unix_socket_state_t *s)
{
    static const char path[] = "/tmp/.X11-unix/X0";
    return s != NULL && s->path_length == sizeof(path) - 1u &&
        memcmp(s->path, path, sizeof(path) - 1u) == 0;
}

static void netd_x11_diag_send(
    netd_unix_socket_state_t *source,
    const netd_unix_socket_state_t *peer,
    const uint8_t *data,
    uint64_t length)
{
    if (source == NULL || data == NULL ||
        (!netd_unix_is_x11_socket(source) &&
         !netd_unix_is_x11_socket(peer)) ||
        source->diag_x11_send_count >= 32u)
        return;
    source->diag_x11_send_count++;
    const uint64_t shown = length < 32u ? length : 32u;
    printf(
        "[netd-x11-diag] pid=%d from=%llu to=%llu bytes=%llu head=",
        source->pid,
        (unsigned long long)source->handle,
        (unsigned long long)(peer != NULL ? peer->handle : 0),
        (unsigned long long)length);
    for (uint64_t i = 0; i < shown; ++i) printf("%02x", data[i]);
    printf("\n");
}
#endif

static uint16_t unix_request_path_length(const netd_unix_path_t *req)
{
    if (req == NULL) return 0;
    if (req->reserved0 != 0 && req->reserved0 <= sizeof(req->path))
        return (uint16_t)req->reserved0;
    if ((req->flags & NETD_UNIX_PATH_ABSTRACT) != 0) return 0;
    uint16_t length = 0;
    while (length < sizeof(req->path) && req->path[length] != 0) length++;
    return length;
}

static int unix_path_matches(
    const netd_unix_socket_state_t *socket,
    const netd_unix_path_t *req)
{
    const uint16_t length = unix_request_path_length(req);
    return socket != NULL && req != NULL && length != 0 &&
        socket->path_length == length &&
        socket->path_abstract ==
            ((req->flags & NETD_UNIX_PATH_ABSTRACT) != 0) &&
        memcmp(socket->path, req->path, length) == 0;
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
    for (const netd_unix_socket_state_t *s = sockets;
         s != NULL;
         s = s->next)
    {
        if (s->notify_fd < 16) continue;
        uint64_t events = PACHA_FD_EVENT_HANGUP;
        if (s->notify_deferred != 0)
            events |= PACHA_FD_EVENT_WRITABLE;
        if (pacha_service_wait_add(
                wait_set, s->notify_fd, events) != 0)
            return -24;
    }
    return 0;
}

void netd_unix_socket_reap_hangups(
    const struct pacha_service_wait_set *wait_set) {
    for (;;) {
        netd_unix_socket_state_t *orphan = NULL;
        for (netd_unix_socket_state_t *s = sockets;
             s != NULL;
             s = s->next)
        {
            if (s->notify_fd >= 16 &&
                (pacha_service_wait_revents(wait_set, s->notify_fd) &
                 PACHA_FD_EVENT_HANGUP) != 0)
            {
                orphan = s;
                break;
            }
        }
        if (orphan == NULL) break;
        const uint64_t handle = orphan->handle;
        force_destroy_socket(orphan);
        printf("[netd] unix_orphan_reap handle=%llu\n",
               (unsigned long long)handle);
    }
}

static netd_unix_socket_state_t *alloc_socket(void) {
    netd_unix_socket_state_t *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        fprintf(stderr,
            "[netd] unix_socket_alloc failed active=%zu stage=state\n",
            socket_count);
        fflush(stderr);
        return NULL;
    }
    s->rx = malloc(NETD_UNIX_RX);
    if (s->rx == NULL) {
        free(s);
        fprintf(stderr,
            "[netd] unix_socket_alloc failed active=%zu stage=rx\n",
            socket_count);
        fflush(stderr);
        return NULL;
    }
    s->handle = NETD_UNIX_HANDLE_BIT | ++next_handle;
    s->refs = 1;
    s->notify_fd = -1;
    s->next = sockets;
    sockets = s;
    socket_count++;
    return s;
}

int netd_unix_socket_open(uint64_t type, uint64_t protocol, int notify_fd, uint64_t *out_handle) {
    if (out_handle == NULL ||
        (type != NETD_SOCK_STREAM && type != NETD_SOCK_SEQPACKET) ||
        protocol != 0 || notify_fd < 16) return -94;
    netd_unix_socket_state_t *s = alloc_socket();
    if (s == NULL) return -12;
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
    if (first == NULL) return -12;
    netd_unix_socket_state_t *second = alloc_socket();
    if (second == NULL) {
        force_destroy_socket(first);
        return -12;
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
#if NETD_UNIX_DIAG
    printf("[netd-unix-diag] op=dup pid=%d handle=%llu peer=%llu refs=%u closed=%u\n",
        s->pid,
        (unsigned long long)s->handle,
        (unsigned long long)s->peer,
        s->refs,
        s->peer_closed);
#endif
    return 0;
}

int netd_unix_socket_attach_wait(uint64_t handle, int notify_fd) {
    netd_unix_socket_state_t *s = find_socket(handle);
    if (s == NULL || notify_fd < 16 || s->notify_fd >= 16) return -22;
    s->notify_fd = notify_fd;
#if NETD_UNIX_DIAG
    printf("[netd-unix-diag] op=attach handle=%llu peer=%llu notify_fd=%d rx=%u closed=%u\n",
        (unsigned long long)s->handle,
        (unsigned long long)s->peer,
        notify_fd,
        s->rx_len,
        s->peer_closed);
#endif
    if (socket_has_data(s) || s->peer_closed ||
        (s->listening && s->pending_head != 0))
        notify_events(s, readable_events(s));
    return 0;
}

int netd_unix_socket_bind(const netd_unix_path_t *req) {
    netd_unix_socket_state_t *s = req ? find_socket(req->handle) : NULL;
    const uint16_t path_length = unix_request_path_length(req);
    if (s == NULL || path_length == 0 ||
        (req->flags & ~NETD_UNIX_PATH_ABSTRACT) != 0)
        return -22;
    for (;;) {
        netd_unix_socket_state_t *conflict = NULL;
        for (netd_unix_socket_state_t *candidate = sockets;
             candidate != NULL;
             candidate = candidate->next)
        {
            if (!candidate->path_bound ||
                !unix_path_matches(candidate, req)) continue;
            if (candidate == s) return -22;
            if ((req->flags & NETD_UNIX_PATH_ABSTRACT) != 0) return -98;
            conflict = candidate;
            break;
        }
        if (conflict == NULL) break;
        force_destroy_socket(conflict);
    }
    memcpy(s->path, req->path, path_length);
    if (path_length < sizeof(s->path)) s->path[path_length] = 0;
    s->path_length = path_length;
    s->path_abstract =
        (req->flags & NETD_UNIX_PATH_ABSTRACT) != 0;
    s->path_bound = 1;
    s->pid = req->pid; s->uid = req->uid; s->gid = req->gid;
#if NETD_UNIX_DIAG
    netd_unix_diag_path("bind", req, s->handle, 0, 0);
#endif
    return 0;
}

int netd_unix_socket_listen(const netd_listen_t *req) {
    netd_unix_socket_state_t *s = req ? find_socket(req->handle) : NULL;
    if (req == NULL || s == NULL || s->path_length == 0 || req->reserved0 != 0) {
        return -22;
    }
    uint32_t backlog = req->backlog > 0 ? (uint32_t)req->backlog : 1u;
    if (backlog > NETD_UNIX_BACKLOG_MAX) backlog = NETD_UNIX_BACKLOG_MAX;
    s->backlog = backlog;
    s->listening = 1;
#if NETD_UNIX_DIAG
    printf("[netd-unix-diag] op=listen handle=%llu backlog=%u abstract=%u path=%.*s\n",
        (unsigned long long)s->handle,
        backlog,
        s->path_abstract,
        (int)s->path_length,
        s->path);
#endif
    return 0;
}

int netd_unix_socket_connect(const netd_unix_path_t *req) {
    netd_unix_socket_state_t *client = req ? find_socket(req->handle) : NULL;
    if (client == NULL) {
#if NETD_UNIX_DIAG
        netd_unix_diag_path("connect", req, 0, 0, -9);
#endif
        return -9;
    }
    netd_unix_socket_state_t *listener = NULL;
    for (netd_unix_socket_state_t *candidate = sockets;
         candidate != NULL;
         candidate = candidate->next)
        if (candidate->listening && unix_path_matches(candidate, req)) {
            listener = candidate;
            break;
        }
    if (listener == NULL) {
#if NETD_UNIX_DIAG
        netd_unix_diag_path("connect", req, 0, 0, -111);
#endif
        return -111;
    }
    if (listener->type != client->type) {
#if NETD_UNIX_DIAG
        netd_unix_diag_path("connect", req, listener->handle, 0, -91);
#endif
        return -91;
    }
    if (listener->pending_count >= listener->backlog) {
#if NETD_UNIX_DIAG
        netd_unix_diag_path("connect", req, listener->handle, 0, -11);
#endif
        return -11;
    }
    netd_unix_socket_state_t *server = alloc_socket();
    if (server == NULL) {
#if NETD_UNIX_DIAG
        netd_unix_diag_path("connect", req, listener->handle, 0, -12);
#endif
        return -12;
    }
    server->type = client->type; server->connected = 1; server->peer = client->handle;
    memcpy(server->path, listener->path, listener->path_length);
    if (listener->path_length < sizeof(server->path))
        server->path[listener->path_length] = 0;
    server->path_length = listener->path_length;
    server->path_abstract = listener->path_abstract;
    server->path_bound = 0;
    server->pid = req->pid; server->uid = req->uid; server->gid = req->gid;
    client->connected = 1; client->peer = server->handle;
    client->pid = req->pid; client->uid = req->uid; client->gid = req->gid;
    if (!listener->path_abstract && listener->path_length >= 10 &&
        memcmp(listener->path, "/tmp/dbus-", 10) == 0) {
        client->reserved0 = 1;
        server->reserved0 = 1;
    }
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
#if NETD_UNIX_DIAG
    netd_unix_diag_path(
        "connect", req, listener->handle, server->handle, 0);
#endif
    notify_events(listener, NETD_POLLIN);
    return 0;
}

int netd_unix_socket_name(netd_unix_name_t *req) {
    if (req == NULL || req->peer > 1u || req->reserved0 != 0) return -22;
    netd_unix_socket_state_t *socket = find_socket(req->handle);
    if (socket == NULL) return -9;
    netd_unix_socket_state_t *named = socket;
    if (req->peer != 0u) {
        if (!socket->connected || socket->peer == 0) return -107;
        named = find_socket(socket->peer);
        if (named == NULL) return -107;
    }
    req->abstract = named->path_abstract != 0;
    req->length = named->path_length;
    memset(req->path, 0, sizeof(req->path));
    if (named->path_length != 0)
        memcpy(req->path, named->path, named->path_length);
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
    if (server == NULL) {
#if NETD_UNIX_DIAG
        printf("[netd-unix-diag] op=accept listener=%llu peer=0 pending=%u status=-11\n",
            (unsigned long long)listener->handle,
            listener->pending_count);
#endif
        return -11;
    }
    req->accepted_handle = server->handle;
    req->pid = server->pid; req->uid = server->uid; req->gid = server->gid;
    if (listener->pending_head != 0)
        notify_events(listener, NETD_POLLIN);
#if NETD_UNIX_DIAG
    printf("[netd-unix-diag] op=accept listener=%llu peer=%llu pending=%u status=0\n",
        (unsigned long long)listener->handle,
        (unsigned long long)server->handle,
        listener->pending_count);
#endif
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
    if (s == NULL || peer == NULL || !s->connected || out_sent == NULL) {
#if NETD_UNIX_DIAG
        printf("[netd-unix-diag] op=send handle=%llu peer=%llu length=%llu connected=%u closed=%u status=-107\n",
            (unsigned long long)(req != NULL ? req->handle : 0),
            (unsigned long long)(s != NULL ? s->peer : 0),
            (unsigned long long)(req != NULL ? req->length : 0),
            s != NULL ? s->connected : 0,
            s != NULL ? s->peer_closed : 0);
#endif
        return -107;
    }
#if NETD_UNIX_DIAG
    if (s->diag_send_count < 4u) {
        s->diag_send_count++;
        printf("[netd-unix-diag] op=send handle=%llu peer=%llu length=%llu peer_rx=%u call=%u status=begin\n",
            (unsigned long long)s->handle,
            (unsigned long long)peer->handle,
            (unsigned long long)req->length,
            peer->rx_len,
            s->diag_send_count);
    }
#endif
    if (!netd_unix_transfer_valid(req, capability_fds, capability_count))
        return -22;
#if NETD_X11_DIAG
    netd_x11_diag_send(s, peer, req->data, req->length);
#endif
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
#if NETD_DBUS_DIAG
    dbus_diag_message(s, peer, req->data, sent);
#endif
    *out_sent = sent;
#if NETD_DBUS_DIAG
    if ((s->reserved0 != 0 || peer->reserved0 != 0) && sent >= 512u &&
        s->diag_dbus_send_count < 64)
    {
        s->diag_dbus_send_count++;
        printf("[netd-dbus-wait] op=send count=%u from=%llu to=%llu bytes=%llu peer_rx=%u peer_pending=%u\n",
            (unsigned)s->diag_dbus_send_count,
            (unsigned long long)s->handle,
            (unsigned long long)peer->handle,
            (unsigned long long)sent,
            peer->rx_len,
            peer->notify_pending);
    }
#endif
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
#if NETD_UNIX_DIAG
    if (s->diag_recv_count < 4u) {
        s->diag_recv_count++;
        printf("[netd-unix-diag] op=recv handle=%llu peer=%llu capacity=%llu rx=%u closed=%u call=%u status=begin\n",
            (unsigned long long)s->handle,
            (unsigned long long)s->peer,
            (unsigned long long)capacity,
            s->rx_len,
            s->peer_closed,
            s->diag_recv_count);
    }
#endif
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
#if NETD_UNIX_DIAG
    if (s->diag_poll_count < 4u) {
        s->diag_poll_count++;
        printf("[netd-unix-diag] op=poll handle=%llu peer=%llu events=%u rx=%u closed=%u call=%u\n",
            (unsigned long long)s->handle,
            (unsigned long long)s->peer,
            events,
            s->rx_len,
            s->peer_closed,
            s->diag_poll_count);
    }
#endif
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
    notification_remove(s);
    netd_unix_socket_state_t **cursor = &sockets;
    while (*cursor != NULL && *cursor != s) cursor = &(*cursor)->next;
    if (*cursor == s) *cursor = s->next;
    if (socket_count != 0) socket_count--;
    free(s->rx);
    s->rx = NULL;
    s->handle = 0;
    free(s);
}

int netd_unix_socket_close(uint64_t handle) {
    netd_unix_socket_state_t *s = find_socket(handle);
    if (s == NULL) return -9;
#if NETD_UNIX_DIAG
    printf("[netd-unix-diag] op=close pid=%d handle=%llu peer=%llu refs=%u rx=%u closed=%u path=%.*s\n",
        s->pid,
        (unsigned long long)s->handle,
        (unsigned long long)s->peer,
        s->refs,
        s->rx_len,
        s->peer_closed,
        (int)s->path_length,
        s->path);
#endif
    if (s->refs > 1) {
        s->refs--;
        return 0;
    }
    char path[sizeof(s->path)];
    memcpy(path, s->path, sizeof(path));
    path[sizeof(path) - 1] = 0;
    force_destroy_socket(s);
    if (path[0] == 0) return 0;
    size_t refs = 0;
    for (const netd_unix_socket_state_t *active = sockets;
         active != NULL;
         active = active->next)
        refs += active->refs;
    printf("[netd] unix_close path=%s active=%zu refs=%zu\n",
           path, socket_count, refs);
    fflush(stdout);
    return 0;
}
