#include "broker.h"
#include "broker_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static struct unix_socket *find_socket(struct unix_broker *broker, uint64_t id)
{
    for (struct unix_socket *socket = broker->sockets; socket; socket = socket->next)
        if (socket->id == id) return socket;
    return NULL;
}

static struct unix_owner *find_owner(struct unix_socket *socket, struct unix_session *session)
{
    for (struct unix_owner *owner = socket->owners; owner; owner = owner->next)
        if (owner->session == session) return owner;
    return NULL;
}

static struct unix_socket *owned(struct unix_broker *broker,
    struct unix_session *session, uint64_t id)
{
    if (broker == NULL || session == NULL) return NULL;
    struct unix_socket *socket = find_socket(broker, id);
    return socket != NULL && find_owner(socket, session) != NULL ? socket : NULL;
}

static int retain(struct unix_socket *socket, struct unix_session *session)
{
    struct unix_owner *owner = find_owner(socket, session);
    if (owner != NULL) {
        if (owner->references == UINT32_MAX) return -EOVERFLOW;
        owner->references++;
        return 0;
    }
    owner = calloc(1, sizeof(*owner));
    if (owner == NULL) return -ENOMEM;
    owner->session = session;
    owner->references = 1;
    owner->next = socket->owners;
    socket->owners = owner;
    return 0;
}

struct unix_socket *unix_broker_lookup(struct unix_broker *broker, uint64_t id)
{
    return find_socket(broker, id);
}

struct unix_route *unix_broker_route_lookup(struct unix_broker *broker, uint64_t id)
{
    for (struct unix_route *route = broker->routes; route; route = route->next)
        if (route->id == id) return route;
    return NULL;
}

struct unix_socket *unix_broker_owned(struct unix_broker *broker,
    struct unix_session *session, uint64_t id)
{
    return owned(broker, session, id);
}

int unix_broker_import_socket(struct unix_broker *broker, struct unix_session *session, uint64_t id)
{
    struct unix_socket *socket = find_socket(broker, id);
    return socket ? retain(socket, session) : -EBADF;
}

static void notify(struct unix_broker *broker, const struct unix_socket *socket)
{
    if (socket && broker->platform.notify)
        broker->platform.notify(broker->platform.context, socket->id);
}

static void release_connection(struct unix_broker *broker, struct unix_connection *connection)
{
    if (--connection->references != 0) return;
    if (broker->platform.connection_destroy)
        broker->platform.connection_destroy(broker->platform.context, connection->directions);
    else for (unsigned i = 0; i < 2; i++)
        broker->platform.direction_destroy(broker->platform.context, &connection->directions[i]);
    free(connection);
}

static void wake_connectors(struct unix_broker *broker, uint64_t listener)
{
    for (struct unix_socket *socket = broker->sockets; socket; socket = socket->next) {
        if (socket->connect_wait_listener != listener) continue;
        socket->connect_wait_listener = 0;
        notify(broker, socket);
    }
}

static void destroy_route(struct unix_broker *broker, struct unix_route *route)
{
    struct unix_route **cursor = &broker->routes;
    while (*cursor != route) cursor = &(*cursor)->next;
    *cursor = route->next;
    broker->platform.direction_destroy(broker->platform.context, &route->direction);
    free(route);
}

static void destroy_socket(struct unix_broker *broker, struct unix_socket *socket)
{
    wake_connectors(broker, socket->id);
    if (broker->platform.socket_destroy) broker->platform.socket_destroy(broker->platform.context, socket->id);
    while (socket->datagram_head) {
        struct unix_datagram *datagram = socket->datagram_head;
        socket->datagram_head = datagram->next;
        datagram->route->queued--;
        free(datagram);
    }
    struct unix_route *route = broker->routes;
    while (route) {
        struct unix_route *next = route->next;
        if (route->source == socket) {
            route->source = NULL;
            unix_transport_shutdown_tx(route->direction.tx);
            notify(broker, route->destination);
        }
        if (route->destination == socket) {
            unix_transport_shutdown_rx(route->direction.rx);
            notify(broker, route->source);
        }
        if (route->destination == socket || (route->source == NULL && route->queued == 0))
            destroy_route(broker, route);
        route = next;
    }
    while (socket->pending_head) {
        struct unix_socket *pending = socket->pending_head;
        socket->pending_head = pending->pending_next;
        destroy_socket(broker, pending);
    }
    if (socket->connection) {
        unix_transport_shutdown_tx(socket->connection->directions[socket->side].tx);
        unix_transport_shutdown_rx(socket->connection->directions[1u - socket->side].rx);
        notify(broker, socket->peer);
        release_connection(broker, socket->connection);
    }
    if (socket->peer) socket->peer->peer = NULL;
    struct unix_socket **cursor = &broker->sockets;
    while (*cursor != socket) cursor = &(*cursor)->next;
    *cursor = socket->next;
    while (socket->owners) {
        struct unix_owner *owner = socket->owners;
        socket->owners = owner->next;
        free(owner);
    }
    broker->socket_count--;
    free(socket);
}

struct unix_broker *unix_broker_create(const struct unix_broker_platform *platform)
{
    if (platform == NULL || platform->direction_create == NULL ||
        platform->direction_destroy == NULL ||
        (!!platform->connection_create != !!platform->connection_destroy)) return NULL;
    struct unix_broker *broker = calloc(1, sizeof(*broker));
    if (broker) {
        broker->platform = *platform;
        broker->next_id = 1;
    }
    return broker;
}

void unix_broker_collect(struct unix_broker *broker)
{
    for (struct unix_socket *socket = broker->sockets; socket; socket = socket->next)
        socket->reachable = socket->owners != NULL;
    unix_rights_mark_roots(broker);
    int changed;
    do {
        changed = 0;
        for (struct unix_socket *socket = broker->sockets; socket; socket = socket->next) {
            if (!socket->reachable) continue;
            for (struct unix_socket *pending = socket->pending_head; pending; pending = pending->pending_next) {
                if (pending->reachable) continue;
                pending->reachable = 1;
                changed = 1;
            }
        }
        changed |= unix_rights_mark_edges(broker);
    } while (changed);
    // Drop unreachable queue references before their socket objects disappear.
    unix_rights_sweep(broker);
    /* GC may encounter a pending child before its listener in the global
     * list. Detach unreachable listener queues before sweeping either one;
     * otherwise the listener destructor would follow already freed children. */
    for (struct unix_socket *socket = broker->sockets; socket; socket = socket->next) {
        if (socket->reachable) continue;
        socket->pending_head = socket->pending_tail = NULL;
        socket->pending_count = 0;
    }
    for (;;) {
        struct unix_socket *victim = broker->sockets;
        while (victim && victim->reachable) victim = victim->next;
        if (!victim) break;
        destroy_socket(broker, victim);
    }
}

void unix_broker_destroy(struct unix_broker *broker)
{
    if (broker == NULL) return;
    /* Sessions remove owned listeners (and their pending children) first. */
    while (broker->sessions) unix_broker_session_destroy(broker, broker->sessions);
    unix_rights_destroy(broker);
    while (broker->sockets) destroy_socket(broker, broker->sockets);
    free(broker);
}

int unix_broker_session_create(struct unix_broker *broker,
    const struct unix_credentials *credentials, struct unix_session **out)
{
    if (broker == NULL || credentials == NULL || out == NULL ||
        credentials->pid <= 0 || credentials->generation == 0) return -EINVAL;
    struct unix_session *session = calloc(1, sizeof(*session));
    if (session == NULL) return -ENOMEM;
    if (broker->next_id == UINT64_MAX) { free(session); return -EOVERFLOW; }
    session->id = broker->next_id++;
    session->credentials = *credentials;
    session->next = broker->sessions;
    broker->sessions = session;
    *out = session;
    return 0;
}

void unix_broker_session_destroy(struct unix_broker *broker, struct unix_session *session)
{
    if (broker == NULL || session == NULL) return;
    unix_rights_session_died(broker, session->id);
    for (struct unix_socket *socket = broker->sockets; socket; socket = socket->next) {
        struct unix_owner **cursor = &socket->owners;
        while (*cursor && (*cursor)->session != session) cursor = &(*cursor)->next;
        if (*cursor == NULL) continue;
        struct unix_owner *owner = *cursor;
        *cursor = owner->next;
        free(owner);
    }
    unix_broker_collect(broker);
    struct unix_session **cursor = &broker->sessions;
    while (*cursor && *cursor != session) cursor = &(*cursor)->next;
    if (*cursor) { *cursor = session->next; free(session); }
}

uint64_t unix_broker_session_id(const struct unix_session *session)
{
    return session != NULL ? session->id : 0;
}

int unix_broker_session_credentials(struct unix_session *session,
    const struct unix_credentials *credentials)
{
    if (session == NULL || credentials == NULL ||
        /* Generation identifies the process incarnation, not a credential
         * revision. Only the manager's ordered private admin channel updates it. */
        credentials->generation != session->credentials.generation ||
        credentials->reserved ||
        credentials->pid != session->credentials.pid) return -EINVAL;
    session->credentials = *credentials;
    return 0;
}

void unix_broker_session_identity(const struct unix_session *session, struct unix_credentials *out)
{
    *out = session->credentials;
}

int unix_broker_check_credentials(const struct unix_session *session,
    const struct unix_credentials *claimed)
{
    if (session == NULL || claimed == NULL) return -EINVAL;
    const struct unix_credentials *actual = &session->credentials;
    if (claimed->generation != actual->generation) return -ESTALE;
    /* The current Linux personality grants no CAP_SETUID/GID/SYS_ADMIN.
     * Root UID alone is not a capability grant. */
    if (claimed->pid != actual->pid ||
        (claimed->uid != actual->uid && claimed->uid != actual->euid && claimed->uid != actual->suid) ||
        (claimed->gid != actual->gid && claimed->gid != actual->egid && claimed->gid != actual->sgid))
        return -EPERM;
    return 0;
}

int unix_broker_socket(struct unix_broker *broker, struct unix_session *session,
    uint32_t type, uint32_t flags, uint64_t *out)
{
    if (broker == NULL || session == NULL || out == NULL) return -EINVAL;
    if (type != UNIX_TRANSPORT_STREAM && type != UNIX_TRANSPORT_DGRAM &&
        type != UNIX_TRANSPORT_SEQPACKET) return -ESOCKTNOSUPPORT;
    if (flags & ~UINT32_C(0x800)) return -EINVAL; /* shared O_NONBLOCK only */
    if (broker->next_id == UINT64_MAX) return -EOVERFLOW;
    struct unix_socket *socket = calloc(1, sizeof(*socket));
    if (socket == NULL) return -ENOMEM;
    const int status = retain(socket, session);
    if (status != 0) { free(socket); return status; }
    socket->id = broker->next_id++;
    socket->type = type;
    socket->flags = flags;
    socket->credentials = session->credentials;
    socket->next = broker->sockets;
    broker->sockets = socket;
    broker->socket_count++;
    *out = socket->id;
    return 0;
}

static int connect_pair(struct unix_broker *broker, struct unix_socket *first,
    struct unix_socket *second)
{
    if (broker->next_id == UINT64_MAX) return -EOVERFLOW;
    struct unix_connection *connection = calloc(1, sizeof(*connection));
    if (connection == NULL) return -ENOMEM;
    connection->generation = broker->next_id++;
    if (broker->platform.connection_create) {
        const int status = broker->platform.connection_create(broker->platform.context,
            connection->generation, first->type, connection->directions);
        if (status) { free(connection); return status; }
    } else for (unsigned i = 0; i < 2; i++) {
        connection->directions[i].tx_fd = connection->directions[i].rx_fd = -1;
        const int status = broker->platform.direction_create(broker->platform.context,
            connection->generation, first->type, &connection->directions[i]);
        if (status != 0) {
            while (i != 0) broker->platform.direction_destroy(broker->platform.context,
                &connection->directions[--i]);
            free(connection);
            return status;
        }
    }
    connection->references = 2;
    first->connection = second->connection = connection;
    first->side = 0;
    second->side = 1;
    connection->directions[0].tx->flags = first->flags;
    connection->directions[1].tx->flags = second->flags;
    connection->directions[1].rx->options = first->options;
    connection->directions[0].rx->options = second->options |
        (second->owners ? 0 : UNIX_SOCKET_PASSCRED); /* before accept, capture credentials */
    first->peer = second;
    second->peer = first;
    first->peer_credentials = second->credentials;
    second->peer_credentials = first->credentials;
    first->peer_address = second->address;
    second->peer_address = first->address;
    return 0;
}

int unix_broker_socketpair(struct unix_broker *broker, struct unix_session *session,
    uint32_t type, uint32_t flags, uint64_t out[2])
{
    if (out == NULL) return -EINVAL;
    out[0] = out[1] = 0;
    int status = unix_broker_socket(broker, session, type, flags, &out[0]);
    if (status != 0) return status;
    status = unix_broker_socket(broker, session, type, flags, &out[1]);
    if (status == 0) {
        struct unix_socket *first = find_socket(broker, out[0]);
        struct unix_socket *second = find_socket(broker, out[1]);
        if (type == UNIX_TRANSPORT_DGRAM) {
            first->dgram_peer = second->id;
            second->dgram_peer = first->id;
            first->peer_credentials = second->credentials;
            second->peer_credentials = first->credentials;
        } else status = connect_pair(broker, first, second);
    }
    if (status != 0) {
        if (out[1]) unix_broker_close(broker, session, out[1]);
        unix_broker_close(broker, session, out[0]);
        out[0] = out[1] = 0;
    }
    return status;
}

static int address_valid(const struct unix_address *address)
{
    if (address == NULL || address->length > UNIX_PATH_BYTES || address->reserved)
        return 0;
    if (address->kind == UNIX_ADDRESS_ABSTRACT)
        return address->filesystem == 0 && address->inode == 0;
    return address->kind == UNIX_ADDRESS_PATH && address->inode != 0;
}

static int address_equal(const struct unix_address *first, const struct unix_address *second)
{
    if (first->kind != second->kind) return 0;
    if (first->kind == UNIX_ADDRESS_PATH)
        return first->filesystem == second->filesystem && first->inode == second->inode;
    return first->kind == UNIX_ADDRESS_ABSTRACT && first->length == second->length &&
        memcmp(first->bytes, second->bytes, first->length) == 0;
}

int unix_broker_bind(struct unix_broker *broker, struct unix_session *session,
    uint64_t id, const struct unix_address *address)
{
    struct unix_socket *socket = owned(broker, session, id);
    if (socket == NULL) return -EBADF;
    if (!address_valid(address) || socket->address.kind != UNIX_ADDRESS_UNNAMED)
        return -EINVAL;
    for (struct unix_socket *other = broker->sockets; other; other = other->next)
        if (other->bound && address_equal(&other->address, address)) return -EADDRINUSE;
    socket->address = *address;
    socket->bound = 1;
    return 0;
}

int unix_broker_path_check(struct unix_broker *broker, struct unix_session *session,
    uint64_t id, int binding)
{
    const struct unix_socket *socket = owned(broker, session, id);
    if (!socket) return -EBADF;
    /* The private filed bridge checks pathname authority against this
     * authenticated session. UID zero is not a namespace capability. */
    if (binding) return socket->bound ? -EINVAL : 0;
    if (socket->connection) return -EISCONN;
    return socket->listening ? -EINVAL : 0;
}

int unix_broker_listen(struct unix_broker *broker, struct unix_session *session,
    uint64_t id, uint32_t backlog)
{
    struct unix_socket *socket = owned(broker, session, id);
    if (socket == NULL) return -EBADF;
    if (socket->type == UNIX_TRANSPORT_DGRAM) return -EOPNOTSUPP;
    if (socket->connection || socket->address.kind == UNIX_ADDRESS_UNNAMED) return -EINVAL;
    socket->backlog = backlog == 0 ? 1 : backlog;
    socket->listening = 1;
    socket->credentials = session->credentials;
    wake_connectors(broker, socket->id);
    return 0;
}

static int autobind(struct unix_broker *broker, struct unix_socket *socket)
{
    if (socket->bound || !(socket->options & UNIX_SOCKET_PASSCRED)) return 0;
    static const uint8_t digits[] = "0123456789abcdef";
    for (unsigned attempt = 0; attempt < 0x100000; attempt++) {
        if (broker->next_id == UINT64_MAX) return -EOVERFLOW;
        uint64_t name = broker->next_id++;
        struct unix_address address = { .kind = UNIX_ADDRESS_ABSTRACT, .length = 5 };
        for (unsigned i = 0; i < 5; i++) { address.bytes[4-i] = digits[name & 15]; name >>= 4; }
        struct unix_socket *other;
        for (other = broker->sockets; other; other = other->next)
            if (other->bound && address_equal(&other->address, &address)) break;
        if (other) continue;
        socket->address = address;
        socket->bound = 1;
        return 0;
    }
    return -ENOSPC;
}

static void purge_datagrams(struct unix_broker *broker, struct unix_socket *socket)
{
    struct unix_socket *peer = find_socket(broker, socket->dgram_peer);
    if (socket->datagram_head && peer && peer != socket && peer->dgram_peer == socket->id) {
        peer->error = ECONNRESET;
        peer->poll_sequence.error = ++broker->next_poll_event;
        notify(broker, peer);
    }
    while (socket->datagram_head) {
        struct unix_datagram *head = socket->datagram_head;
        struct unix_route *route = head->route;
        const uint32_t after = head->delivery.position +
            ((head->delivery.length + sizeof(struct unix_record) + 31u) & ~31u);
        /* Only broker-owned delivery metadata determines the discarded range.
         * Do not steal a live reader's lock. Its later FINISH must fail the
         * delivery-ID check, and its hidden FD imports will be rolled back.
         * The mappings remain pinned by the reader's capabilities. */
        const uint64_t before = __atomic_load_n(&route->direction.rx->consumed, __ATOMIC_SEQ_CST);
        /* All DGRAM consume/close commits run on this broker thread. The
         * client only acquires/cancels RX; hostile cursor writes must not
         * force the daemon into an unbounded compare-exchange retry loop. */
        __atomic_store_n(&route->direction.rx->consumed,
            after | (before & UNIX_TRANSPORT_CLOSED), __ATOMIC_SEQ_CST);
        __atomic_fetch_add(&route->direction.rx->changes, 1, __ATOMIC_SEQ_CST);
        if (head->delivery.ticket) unix_rights_drop_datagram(broker, head->delivery.ticket);
        socket->datagram_head = head->next;
        route->queued--;
        free(head);
        if (!route->source && !route->queued) destroy_route(broker, route);
    }
    socket->datagram_tail = NULL;
    socket->datagram_bytes = 0;
    socket->space_sequence = ++broker->next_poll_event;
    for (struct unix_route *route = broker->routes; route; route = route->next)
        if (route->destination == socket) notify(broker, route->source);
    notify(broker, socket);
}

int unix_broker_connect(struct unix_broker *broker, struct unix_session *session,
    uint64_t id, const struct unix_address *address)
{
    struct unix_socket *socket = owned(broker, session, id);
    if (socket == NULL) return -EBADF;
    socket->connect_wait_listener = 0;
    if (socket->type == UNIX_TRANSPORT_DGRAM && address && address->kind == UNIX_ADDRESS_UNNAMED) {
        if (socket->dgram_peer) purge_datagrams(broker, socket);
        socket->dgram_peer = 0;
        socket->poll_sequence.write = ++broker->next_poll_event;
        socket->peer_address = (struct unix_address){0};
        unix_broker_collect(broker);
        return 0;
    }
    if (!address_valid(address)) return -EINVAL;
    if (socket->connection) return -EISCONN;
    if (socket->listening) return -EINVAL;
    int bind_status = autobind(broker, socket);
    if (bind_status) return bind_status;
    struct unix_socket *listener = NULL;
    for (struct unix_socket *other = broker->sockets; other; other = other->next)
        if (other->bound && address_equal(&other->address, address)) { listener = other; break; }
    if (listener == NULL) return -ECONNREFUSED;
    if (listener->type != socket->type) return -EPROTOTYPE;
    if (socket->type == UNIX_TRANSPORT_DGRAM) {
        if (listener->dgram_peer && listener->dgram_peer != socket->id) return -EPERM;
        if (socket->dgram_peer && socket->dgram_peer != listener->id)
            purge_datagrams(broker, socket);
        socket->dgram_peer = listener->id;
        socket->poll_sequence.write = ++broker->next_poll_event;
        socket->peer_address = listener->address;
        /* Named DGRAM connect does not establish SO_PEERCRED. Only
         * socketpair snapshots peer credentials for datagrams. */
        unix_broker_collect(broker);
        return 0;
    }
    if (!listener->listening) return -ECONNREFUSED;
    if (listener->pending_count >= listener->backlog) {
        socket->connect_wait_listener = listener->id;
        return -EAGAIN;
    }
    if (broker->next_id == UINT64_MAX) return -EOVERFLOW;
    struct unix_socket *accepted = calloc(1, sizeof(*accepted));
    if (accepted == NULL) return -ENOMEM;
    accepted->id = broker->next_id++;
    accepted->type = socket->type;
    accepted->address = listener->address;
    accepted->credentials = listener->credentials;
    accepted->options = listener->options;
    socket->credentials = session->credentials;
    const int status = connect_pair(broker, socket, accepted);
    if (status != 0) { free(accepted); return status; }
    accepted->next = broker->sockets;
    broker->sockets = accepted;
    broker->socket_count++;
    if (listener->pending_tail) listener->pending_tail->pending_next = accepted;
    else listener->pending_head = accepted;
    listener->pending_tail = accepted;
    listener->pending_count++;
    listener->poll_sequence.read = ++broker->next_poll_event;
    notify(broker, listener);
    return 0;
}

int unix_broker_accept(struct unix_broker *broker, struct unix_session *session,
    uint64_t id, uint64_t *out)
{
    struct unix_socket *socket = owned(broker, session, id);
    if (socket == NULL) return -EBADF;
    if (out == NULL || !socket->listening) return -EINVAL;
    struct unix_socket *accepted = socket->pending_head;
    if (accepted == NULL) return -EAGAIN;
    const int status = retain(accepted, session);
    if (status != 0) return status;
    __atomic_store_n(&accepted->connection->directions[1u - accepted->side].rx->options,
        accepted->options, __ATOMIC_RELEASE);
    socket->pending_head = accepted->pending_next;
    if (socket->pending_head == NULL) socket->pending_tail = NULL;
    accepted->pending_next = NULL;
    socket->pending_count--;
    wake_connectors(broker, socket->id);
    *out = accepted->id;
    notify(broker, socket);
    return 0;
}

int unix_broker_name(struct unix_broker *broker, struct unix_session *session,
    uint64_t id, int peer, struct unix_address *out)
{
    struct unix_socket *socket = owned(broker, session, id);
    if (socket == NULL) return -EBADF;
    if (out == NULL) return -EINVAL;
    if (peer && socket->connection == NULL && socket->dgram_peer == 0) return -ENOTCONN;
    *out = peer ? socket->peer_address : socket->address;
    return 0;
}

int unix_broker_socket_attachment(struct unix_socket *socket,
    struct unix_attachment *out, struct unix_direction *tx,
    struct unix_direction *rx)
{
    if (socket == NULL) return -EBADF;
    if (out == NULL || tx == NULL || rx == NULL) return -EINVAL;
    *out = (struct unix_attachment){ .socket = socket->id,
        .generation = socket->connection ? socket->connection->generation : 0,
        .peer = socket->peer ? socket->peer->id : 0,
        .type = socket->type, .flags = socket->flags, .listening = socket->listening,
        .peer_credentials = socket->peer_credentials };
    if (!socket->connection) { *tx = (struct unix_direction){0}; *rx = (struct unix_direction){0}; return 0; }
    *tx = socket->connection->directions[socket->side];
    *rx = socket->connection->directions[1u - socket->side];
    out->flags = __atomic_load_n(&tx->tx->flags, __ATOMIC_ACQUIRE);
    return 0;
}

int unix_broker_attach(struct unix_broker *broker, struct unix_session *session,
    uint64_t id, struct unix_attachment *out, struct unix_direction *tx, struct unix_direction *rx)
{
    struct unix_socket *socket = owned(broker, session, id);
    if (!socket) return -EBADF;
    if (!socket->connection) return -ENOTCONN;
    return unix_broker_socket_attachment(socket, out, tx, rx);
}

int unix_broker_poll_sequence(struct unix_broker *broker, struct unix_session *session,
    uint64_t id, struct unix_poll_sequence *out)
{
    const struct unix_socket *socket = owned(broker, session, id);
    if (!socket) return -EBADF;
    *out = socket->poll_sequence;
    const struct unix_socket *peer = socket->dgram_peer ? find_socket(broker, socket->dgram_peer) : NULL;
    if (peer && peer->space_sequence > out->write) out->write = peer->space_sequence;
    return 0;
}

int unix_broker_poll(struct unix_broker *broker, struct unix_session *session,
    uint64_t id, uint32_t events, uint64_t *out)
{
    const struct unix_socket *socket = owned(broker, session, id);
    if (!socket) return -EBADF;
    if (socket->type == UNIX_TRANSPORT_DGRAM) {
        uint32_t ready = socket->datagram_head ? events & 1u : 0;
        if (socket->error) ready |= 8u; /* POLLERR, independent of requested mask */
        if (socket->shutdown & 1u) ready |= events & (1u | 0x2000u);
        if (socket->shutdown == 3u) ready |= 0x10u;
        const struct unix_socket *peer = socket->dgram_peer ? find_socket(broker, socket->dgram_peer) : NULL;
        /* Unconnected sendto chooses its destination at send time. A missing
         * connected peer is writable so the following send reports its error. */
        if (socket->error || (socket->shutdown & 2u) || !peer || (peer->shutdown & 1u) ||
            peer->datagram_bytes <= UNIX_TRANSPORT_BYTES - 64u) ready |= events & 4u;
        *out = ready;
        return 0;
    }
    if (socket->listening) {
        *out = socket->pending_count ? events & 1u : 0;
        return 0;
    }
    /* Connected data readiness belongs to the shared transport, not RPC. */
    if (socket->connection) return -EISCONN;
    *out = 0x10u | (events & 4u);
    return 0;
}

static struct unix_direction_diagnostic diagnose_direction(
    const struct unix_direction *direction, uint64_t generation)
{
    return (struct unix_direction_diagnostic){
        .generation = generation,
        .published = __atomic_load_n(&direction->tx->published, __ATOMIC_ACQUIRE),
        .consumed = __atomic_load_n(&direction->rx->consumed, __ATOMIC_ACQUIRE),
        .writer = __atomic_load_n(&direction->tx->owner, __ATOMIC_ACQUIRE),
        .reader = __atomic_load_n(&direction->rx->owner, __ATOMIC_ACQUIRE),
    };
}

int unix_broker_pending(struct unix_broker *broker, struct unix_session *session,
    uint64_t id, uint64_t *out)
{
    const struct unix_socket *socket = owned(broker, session, id);
    if (!socket) return -EBADF;
    if (!out) return -EINVAL;
    if (socket->type != UNIX_TRANSPORT_DGRAM) return -EOPNOTSUPP;
    *out = socket->datagram_head ? socket->datagram_head->delivery.length : 0;
    return 0;
}

int unix_broker_can_notify(struct unix_broker *broker, struct unix_session *session,
    uint64_t source, uint64_t destination)
{
    const struct unix_socket *socket = owned(broker, session, source);
    if (!socket) return 0;
    if (socket->id == destination || (socket->peer && socket->peer->id == destination)) return 1;
    for (const struct unix_route *route = broker->routes; route; route = route->next)
        if ((route->source == socket && route->destination->id == destination) ||
            (route->destination == socket && route->source && route->source->id == destination)) return 1;
    return 0;
}

void unix_broker_wait_remove(struct unix_broker *broker, uint64_t id,
    uint32_t notification)
{
    struct unix_socket *socket = find_socket(broker, id);
    if (!socket) return;
    if (socket->connection) {
        unix_wait_remove(&socket->connection->directions[socket->side].tx->waiters, notification);
        unix_wait_remove(&socket->connection->directions[1u - socket->side].rx->waiters, notification);
    }
    for (struct unix_route *route = broker->routes; route; route = route->next) {
        if (route->source == socket) unix_wait_remove(&route->direction.tx->waiters, notification);
        if (route->destination == socket) unix_wait_remove(&route->direction.rx->waiters, notification);
    }
}

int unix_broker_diagnose(struct unix_broker *broker, struct unix_session *session,
    uint64_t id, struct unix_socket_diagnostic *out)
{
    struct unix_socket *socket = owned(broker, session, id);
    if (socket == NULL) return -EBADF;
    if (out == NULL) return -EINVAL;
    *out = (struct unix_socket_diagnostic){
        .socket = id, .peer = socket->peer ? socket->peer->id : socket->dgram_peer,
        .session = session->id, .type = socket->type, .flags = socket->flags,
        .bound = socket->bound, .listening = socket->listening,
        .backlog = socket->backlog, .pending = socket->pending_count,
        .datagram_bytes = socket->datagram_bytes,
    };
    for (struct unix_owner *owner = socket->owners; owner; owner = owner->next) {
        out->owners++;
        out->references += owner->references;
    }
    for (struct unix_datagram *packet = socket->datagram_head; packet; packet = packet->next)
        out->datagram_count++;
    if (socket->connection) {
        out->outgoing = diagnose_direction(
            &socket->connection->directions[socket->side], socket->connection->generation);
        out->incoming = diagnose_direction(
            &socket->connection->directions[1u - socket->side], socket->connection->generation);
    } else if (socket->datagram_head) {
        const struct unix_datagram *head = socket->datagram_head;
        out->delivery = head->delivery.id;
        out->operation = head->delivery.operation;
        out->incoming = diagnose_direction(&head->route->direction, head->route->id);
    }
    return 0;
}

int unix_broker_retain(struct unix_broker *broker, struct unix_session *source,
    uint64_t id, struct unix_session *destination)
{
    struct unix_socket *socket = owned(broker, source, id);
    if (socket == NULL) return -EBADF;
    return destination != NULL ? retain(socket, destination) : -EINVAL;
}

int unix_broker_close(struct unix_broker *broker, struct unix_session *session, uint64_t id)
{
    struct unix_socket *socket = owned(broker, session, id);
    if (socket == NULL) return -EBADF;
    struct unix_owner **cursor = &socket->owners;
    while ((*cursor)->session != session) cursor = &(*cursor)->next;
    struct unix_owner *owner = *cursor;
    if (--owner->references == 0) {
        *cursor = owner->next;
        free(owner);
    }
    unix_broker_collect(broker);
    return 0;
}

int unix_broker_shutdown(struct unix_broker *broker, struct unix_session *session,
    uint64_t id, unsigned how)
{
    struct unix_socket *socket = owned(broker, session, id);
    if (socket == NULL) return -EBADF;
    if (how > 2) return -EINVAL;
    if (socket->type == UNIX_TRANSPORT_DGRAM) {
        socket->shutdown |= how == 0 ? 1u : how == 1 ? 2u : 3u;
        socket->poll_sequence.read = socket->poll_sequence.write =
            socket->space_sequence = ++broker->next_poll_event;
        /* DGRAM shutdown does not turn the peer's receive queue into EOF,
         * nor discard packets already queued on this socket. */
        notify(broker, socket);
        for (struct unix_route *route = broker->routes; route; route = route->next) {
            /* Stop reservations as well as COMMIT, so a writer blocked on a
             * full route observes shutdown without first finding free space.
             * DGRAM readers use HEAD metadata, not TX's EOF flag; RX remains
             * consumable for packets queued before receive shutdown. */
            if (((socket->shutdown & 2u) && route->source == socket) ||
                ((socket->shutdown & 1u) && route->destination == socket))
                unix_transport_shutdown_tx(route->direction.tx);
            if (route->destination == socket) notify(broker, route->source);
            if (route->source == socket) notify(broker, route->destination);
        }
        return 0;
    }
    if (socket->connection == NULL) return -ENOTCONN;
    if (how != 1) unix_transport_shutdown_rx(socket->connection->directions[1u - socket->side].rx);
    if (how != 0) unix_transport_shutdown_tx(socket->connection->directions[socket->side].tx);
    notify(broker, socket);
    notify(broker, socket->peer);
    return 0;
}

int unix_broker_options(struct unix_broker *broker, struct unix_session *session,
    uint64_t id, uint32_t mask, uint32_t values, uint64_t *out)
{
    struct unix_socket *socket = owned(broker, session, id);
    if (!socket) return -EBADF;
    if (!out || (mask & ~(UNIX_SOCKET_REUSEADDR | UNIX_SOCKET_KEEPALIVE | UNIX_SOCKET_PASSCRED)) ||
        (values & ~mask)) return -EINVAL;
    socket->options = (socket->options & ~mask) | values;
    if (socket->connection)
        __atomic_store_n(&socket->connection->directions[1u - socket->side].rx->options,
            socket->options, __ATOMIC_RELEASE);
    *out = socket->options;
    return 0;
}

int unix_broker_peercred(struct unix_broker *broker, struct unix_session *session,
    uint64_t id, struct unix_credentials *out)
{
    struct unix_socket *socket = owned(broker, session, id);
    if (!socket) return -EBADF;
    if (!out) return -EINVAL;
    *out = socket->listening ? socket->credentials :
        socket->peer_credentials.generation ? socket->peer_credentials :
        (struct unix_credentials){ .uid = UINT32_MAX, .gid = UINT32_MAX,
            .euid = UINT32_MAX, .egid = UINT32_MAX };
    return 0;
}

int unix_broker_error(struct unix_broker *broker, struct unix_session *session,
    uint64_t id, uint64_t *out)
{
    struct unix_socket *socket = owned(broker, session, id);
    if (!socket) return -EBADF;
    if (!out) return -EINVAL;
    *out = socket->error;
    socket->error = 0;
    return 0;
}

static int take_error(struct unix_socket *socket)
{
    int error = (int)socket->error;
    socket->error = 0;
    return -error;
}

int unix_broker_get_flags(struct unix_broker *broker, struct unix_session *session,
    uint64_t id, uint64_t *out)
{
    struct unix_socket *socket = owned(broker, session, id);
    if (!socket) return -EBADF;
    *out = socket->connection ? __atomic_load_n(&socket->connection->directions[socket->side].tx->flags,
        __ATOMIC_ACQUIRE) : socket->flags;
    return 0;
}

int unix_broker_set_flags(struct unix_broker *broker, struct unix_session *session,
    uint64_t id, uint32_t flags)
{
    struct unix_socket *socket = owned(broker, session, id);
    if (socket == NULL) return -EBADF;
    if (flags & ~UINT32_C(0x800)) return -EINVAL;
    socket->flags = flags;
    if (socket->connection)
        __atomic_store_n(&socket->connection->directions[socket->side].tx->flags,
            flags, __ATOMIC_RELEASE);
    return 0;
}

void unix_broker_thread_died(struct unix_broker *broker, uint64_t owner)
{
    if (broker == NULL) return;
    unix_rights_thread_died(broker, owner);
    for (struct unix_route *route = broker->routes; route; route = route->next) {
        unix_transport_recover_tx(route->direction.tx, owner);
        unix_transport_recover_rx(route->direction.rx, owner);
        notify(broker, route->source);
        notify(broker, route->destination);
    }
    for (struct unix_socket *socket = broker->sockets; socket; socket = socket->next) {
        if (socket->connection == NULL) continue;
        unix_transport_recover_tx(socket->connection->directions[socket->side].tx, owner);
        unix_transport_recover_rx(socket->connection->directions[1u - socket->side].rx, owner);
        /* Also covers death after publication/unlock but before notification. */
        notify(broker, socket);
    }
    unix_broker_collect(broker);
}

size_t unix_broker_socket_count(const struct unix_broker *broker)
{
    return broker != NULL ? broker->socket_count : 0;
}

int unix_broker_dgram_route(struct unix_broker *broker, struct unix_session *session,
    uint64_t id, const struct unix_address *address, uint64_t *out_route,
    uint64_t *generation, struct unix_direction *direction)
{
    struct unix_socket *socket = owned(broker, session, id);
    if (socket == NULL) return -EBADF;
    if (socket->type != UNIX_TRANSPORT_DGRAM || out_route == NULL ||
        generation == NULL || direction == NULL) return -EINVAL;
    if (socket->error) return take_error(socket);
    if (socket->shutdown & 2u) return -EPIPE;
    int bind_status = autobind(broker, socket);
    if (bind_status) return bind_status;
    struct unix_socket *destination = NULL;
    if (address && address->kind != UNIX_ADDRESS_UNNAMED) {
        if (!address_valid(address)) return -EINVAL;
        for (struct unix_socket *other = broker->sockets; other; other = other->next)
            if (other->bound && address_equal(&other->address, address)) {
                destination = other;
                break;
            }
    } else {
        if (socket->dgram_peer == 0) return -ENOTCONN;
        destination = find_socket(broker, socket->dgram_peer);
    }
    if (destination == NULL) return -ECONNREFUSED;
    if (destination->type != UNIX_TRANSPORT_DGRAM) return -EPROTOTYPE;
    if (destination->shutdown & 1u) return -EPIPE;
    if (destination->dgram_peer != 0 && destination->dgram_peer != socket->id) return -EPERM;
    struct unix_route *route;
    for (route = broker->routes; route; route = route->next)
        if (route->source == socket && route->destination == destination) break;
    if (route == NULL) {
        if (broker->next_id == UINT64_MAX) return -EOVERFLOW;
        route = calloc(1, sizeof(*route));
        if (route == NULL) return -ENOMEM;
        route->id = broker->next_id++;
        const int status = broker->platform.direction_create(broker->platform.context,
            route->id, UNIX_TRANSPORT_DGRAM, &route->direction);
        if (status != 0) { free(route); return status; }
        route->source = socket;
        route->destination = destination;
        route->next = broker->routes;
        broker->routes = route;
    }
    *out_route = *generation = route->id;
    *direction = route->direction;
    return 0;
}

int unix_broker_dgram_commit(struct unix_broker *broker, struct unix_session *session,
    uint64_t route_id, struct unix_write *write, uint64_t *delivery)
{
    if (broker == NULL || session == NULL || write == NULL || delivery == NULL ||
        write->owner == 0 || write->owner == UNIX_TRANSPORT_RECOVERING) return -EINVAL;
    struct unix_route *route;
    for (route = broker->routes; route; route = route->next)
        if (route->id == route_id) break;
    if (route == NULL || route->source == NULL || !find_owner(route->source, session)) return -EBADF;
    const struct unix_record *record = (const void *)(route->direction.tx->data +
        ((uint32_t)write->before & (UNIX_TRANSPORT_BYTES - 1u) & ~31u));
    return unix_broker_dgram_enqueue(broker, route, write, 0,
        __atomic_load_n(&record->operation, __ATOMIC_RELAXED), &session->credentials, delivery);
}

int unix_broker_dgram_enqueue(struct unix_broker *broker, struct unix_route *route,
    struct unix_write *write, uint64_t ticket, uint64_t operation, const struct unix_credentials *credentials,
    uint64_t *delivery)
{
    if (!route || !route->source || !write || !delivery || !credentials) return -EINVAL;
    struct unix_socket *destination = route->destination;
    if (route->source->error) return take_error(route->source);
    if ((route->source->shutdown & 2u) || (destination->shutdown & 1u)) return -EPIPE;
    if (destination->dgram_peer != 0 && destination->dgram_peer != route->source->id) return -EPERM;
    if (write->length > UNIX_TRANSPORT_BYTES - sizeof(struct unix_record) ||
        (write->before & ~UINT64_C(0xffffffff)) || ((uint32_t)write->before & 31u)) return -EPROTO;
    const uint32_t charge = (write->length + sizeof(struct unix_record) + 31u) & ~31u;
    if (write->after != (uint32_t)((uint32_t)write->before + charge)) return -EPROTO;
    if (destination->datagram_bytes > UNIX_TRANSPORT_BYTES - charge) return -EAGAIN;
    struct unix_tx *tx = route->direction.tx;
    const uint64_t consumed = __atomic_load_n(&route->direction.rx->consumed, __ATOMIC_ACQUIRE);
    if (consumed & UNIX_TRANSPORT_CLOSED) return -EPIPE;
    const uint32_t used = (uint32_t)write->before - (uint32_t)consumed;
    if (((uint32_t)consumed & 31u) || used > UNIX_TRANSPORT_BYTES ||
        charge > UNIX_TRANSPORT_BYTES - used) return -EPROTO;
    const struct unix_record *record = (const void *)(tx->data +
        ((uint32_t)write->before & (UNIX_TRANSPORT_BYTES - 1u)));
    if (__atomic_load_n(&record->length, __ATOMIC_RELAXED) != write->length ||
        __atomic_load_n(&record->reserved, __ATOMIC_RELAXED) != 0 ||
        __atomic_load_n(&record->reserved1, __ATOMIC_RELAXED) != 0 ||
        __atomic_load_n(&record->ticket, __ATOMIC_RELAXED) != ticket) return -EPROTO;
    /* FD-bearing sends enter through the rights transaction path. A plain
     * datagram cannot smuggle a ticket into receiver-controlled metadata. */
    if (broker->next_id == UINT64_MAX) return -EOVERFLOW;
    struct unix_datagram *datagram = calloc(1, sizeof(*datagram));
    if (datagram == NULL) return -ENOMEM;
    datagram->delivery = (struct unix_delivery){ .id = broker->next_id++,
        .route = route->id, .generation = route->id, .length = write->length,
        .position = (uint32_t)write->before,
        .operation = operation,
        .ticket = ticket,
        .source = route->source->address, .credentials = *credentials };
    if (!ticket && !((route->source->options | destination->options) & UNIX_SOCKET_PASSCRED))
        datagram->delivery.credentials.generation = 0;
    datagram->route = route;
    const int status = unix_transport_write_commit(tx, write);
    if (status != 0) { free(datagram); return status; }
    if (destination->datagram_tail) destination->datagram_tail->next = datagram;
    else destination->datagram_head = datagram;
    destination->datagram_tail = datagram;
    destination->datagram_bytes += charge;
    destination->poll_sequence.read = ++broker->next_poll_event;
    route->queued++;
    *delivery = datagram->delivery.id;
    notify(broker, destination);
    notify(broker, route->source);
    return 0;
}

int unix_broker_dgram_head(struct unix_broker *broker, struct unix_session *session,
    uint64_t id, struct unix_delivery *delivery, struct unix_direction *direction)
{
    struct unix_socket *socket = owned(broker, session, id);
    if (socket == NULL) return -EBADF;
    if (socket->type != UNIX_TRANSPORT_DGRAM || delivery == NULL || direction == NULL) return -EINVAL;
    if (socket->error) return take_error(socket);
    struct unix_datagram *head = socket->datagram_head;
    if (head == NULL) {
        if (!(socket->shutdown & 1u)) return -EAGAIN;
        *delivery = (struct unix_delivery){0};
        *direction = (struct unix_direction){0};
        return 0; /* EOF is distinct from a zero-length packet's nonzero ID. */
    }
    *delivery = head->delivery;
    *direction = head->route->direction;
    return 0;
}

int unix_broker_dgram_consume(struct unix_broker *broker, struct unix_session *session,
    uint64_t id, uint64_t delivery, struct unix_read *read, int peek)
{
    struct unix_socket *socket = owned(broker, session, id);
    if (socket == NULL) return -EBADF;
    if (socket->type != UNIX_TRANSPORT_DGRAM || read == NULL || delivery == 0) return -EINVAL;
    struct unix_datagram *head = socket->datagram_head;
    if (head == NULL || head->delivery.id != delivery)
        /* A dropped packet is not a completed receive. A high-water mark
         * would incorrectly acknowledge it after a newer packet is read. */
        return delivery == socket->consumed_delivery ? 0 : -ESTALE;
    /* FD-bearing data must pass through the rights transaction, including
     * take=0 for plain read. Never silently orphan the escrow. */
    if (head->delivery.ticket) return -EPROTO;
    return unix_broker_dgram_finish(broker, socket, read, peek);
}

int unix_broker_dgram_finish(struct unix_broker *broker, struct unix_socket *socket,
    struct unix_read *read, int peek)
{
    struct unix_datagram *head = socket->datagram_head;
    if (!head || !read || (peek != 0 && peek != 1)) return -EINVAL;
    if (read->owner == 0 || read->owner == UNIX_TRANSPORT_RECOVERING ||
        read->before != head->delivery.position ||
        read->message_length != head->delivery.length ||
        read->length > head->delivery.length ||
        read->after != (uint32_t)((uint32_t)read->before +
            ((head->delivery.length + sizeof(struct unix_record) + 31u) & ~31u))) return -EPROTO;
    struct unix_route *route = head->route;
    if (__atomic_load_n(&route->direction.rx->owner, __ATOMIC_SEQ_CST) != read->owner ||
        __atomic_load_n(&route->direction.rx->consumed, __ATOMIC_SEQ_CST) != read->before) return -ESTALE;
    if (peek) {
        unix_transport_read_cancel(route->direction.rx, read);
        return 0;
    }
    const int status = unix_transport_read_commit(route->direction.rx, read);
    if (status != 0) return status;
    socket->datagram_head = head->next;
    if (socket->datagram_head == NULL) socket->datagram_tail = NULL;
    socket->datagram_bytes -= (head->delivery.length + sizeof(struct unix_record) + 31u) & ~31u;
    socket->consumed_delivery = head->delivery.id;
    socket->space_sequence = ++broker->next_poll_event;
    route->queued--;
    free(head);
    /* Capacity belongs to the destination, not only the consumed route. */
    for (struct unix_route *pending = broker->routes; pending; pending = pending->next)
        if (pending->destination == socket) notify(broker, pending->source);
    notify(broker, socket);
    if (route->source == NULL && route->queued == 0) destroy_route(broker, route);
    return 0;
}
