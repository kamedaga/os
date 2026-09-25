#include "rights.h"
#include "broker_internal.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define UNIX_TICKET_LIMIT 1024u

struct unix_ticket {
    struct unix_ticket *next;
    struct unix_rights_info info;
    uint64_t sender_session;
    uint64_t owner;
    uint64_t receiver_session;
    uint64_t receiver_owner;
    uint64_t receive_operation;
    uint64_t after;
    uint64_t route;
    uint64_t delivery;
    unsigned expected;
    unsigned taken;
    unsigned sender_acked;
    unsigned receiver_acked;
    struct unix_right_ref refs[];
};

struct unix_right_progress {
    struct unix_right_progress *next;
    uint64_t session;
    uint64_t owner;
    uint64_t sent;
    uint64_t received;
    uint64_t receive_ticket;
    uint64_t receive_socket;
    unsigned take;
    unsigned peek;
    unsigned pending;
    unsigned dead;
};

static void retire_progress(struct unix_broker *broker, struct unix_right_progress *item)
{
    struct unix_right_progress **cursor = &broker->rights_progress;
    while (*cursor != item) cursor = &(*cursor)->next;
    *cursor = item->next;
    free(item);
}

static struct unix_right_progress *progress(struct unix_broker *broker,
    struct unix_session *session, uint64_t owner, int create)
{
    for (struct unix_right_progress *item = broker->rights_progress; item; item = item->next)
        if (item->owner == owner && item->session == session->id) return item;
    if (!create) return NULL;
    struct unix_right_progress *item = calloc(1, sizeof(*item));
    if (!item) return NULL;
    item->session = session->id;
    item->owner = owner;
    item->next = broker->rights_progress;
    broker->rights_progress = item;
    return item;
}

static int can_forget(const struct unix_ticket *ticket)
{
    const int sender_done = !ticket->sender_session || ticket->sender_acked;
    return ((ticket->info.state == UNIX_TICKET_CANCELLED ||
            ticket->info.state == UNIX_TICKET_DROPPED) && sender_done) ||
        (ticket->info.state == UNIX_TICKET_DELIVERED && sender_done &&
            (!ticket->receiver_session || ticket->receiver_acked));
}

static struct unix_ticket *find_ticket(struct unix_broker *broker, uint64_t id)
{
    for (struct unix_ticket *ticket = broker->tickets; ticket; ticket = ticket->next)
        if (ticket->info.ticket == id) return ticket;
    return NULL;
}

static void release_refs(struct unix_broker *broker, struct unix_ticket *ticket)
{
    for (unsigned i = 0; i < ticket->info.count; i++)
        if (ticket->refs[i].external)
            broker->platform.external_release(broker->platform.context, ticket->refs[i].external);
    ticket->info.count = 0;
}

static void free_ticket(struct unix_broker *broker, struct unix_ticket *ticket)
{
    struct unix_ticket **cursor = &broker->tickets;
    while (*cursor != ticket) cursor = &(*cursor)->next;
    *cursor = ticket->next;
    release_refs(broker, ticket);
    broker->ticket_count--;
    free(ticket);
}

void unix_rights_drop_datagram(struct unix_broker *broker, uint64_t id)
{
    struct unix_ticket *ticket = find_ticket(broker, id);
    if (!ticket || ticket->info.state != UNIX_TICKET_QUEUED) return;
    release_refs(broker, ticket);
    ticket->info.state = UNIX_TICKET_DROPPED;
    /* Keep the successful sender receipt until ACK; queue discard must not
     * turn an already committed send into a failed replay. GC follows purge. */
}

static void signal_socket(struct unix_broker *broker, uint64_t id)
{
    if (broker->platform.notify) broker->platform.notify(broker->platform.context, id);
}

static int mark(struct unix_broker *broker, uint64_t id)
{
    struct unix_socket *socket = unix_broker_lookup(broker, id);
    if (!socket || socket->reachable) return 0;
    socket->reachable = 1;
    return 1;
}

void unix_rights_mark_roots(struct unix_broker *broker)
{
    for (struct unix_ticket *ticket = broker->tickets; ticket; ticket = ticket->next) {
        if (ticket->info.state != UNIX_TICKET_PREPARING) continue;
        /* An in-flight send pins its source OFD and prepared rights. It does
         * not pin the peer: peer close must still make COMMIT fail EPIPE. */
        (void)mark(broker, ticket->info.source);
        for (unsigned i = 0; i < ticket->info.count; i++)
            if (ticket->refs[i].socket) (void)mark(broker, ticket->refs[i].socket);
    }
}

int unix_rights_mark_edges(struct unix_broker *broker)
{
    int changed = 0;
    for (struct unix_ticket *ticket = broker->tickets; ticket; ticket = ticket->next) {
        if (ticket->info.state != UNIX_TICKET_QUEUED) continue;
        struct unix_socket *destination = unix_broker_lookup(broker, ticket->info.destination);
        if (!destination || !destination->reachable) continue;
        for (unsigned i = 0; i < ticket->info.count; i++)
            if (ticket->refs[i].socket) changed |= mark(broker, ticket->refs[i].socket);
    }
    return changed;
}

void unix_rights_sweep(struct unix_broker *broker)
{
    struct unix_ticket *ticket = broker->tickets;
    while (ticket) {
        struct unix_ticket *next = ticket->next;
        if (ticket->info.state == UNIX_TICKET_QUEUED) {
            struct unix_socket *destination = unix_broker_lookup(broker, ticket->info.destination);
            if (!destination || !destination->reachable) {
                release_refs(broker, ticket);
                ticket->info.state = UNIX_TICKET_DROPPED;
            }
        }
        if (can_forget(ticket)) free_ticket(broker, ticket);
        ticket = next;
    }
}

void unix_rights_session_died(struct unix_broker *broker, uint64_t session)
{
    struct unix_ticket *ticket = broker->tickets;
    while (ticket) {
        struct unix_ticket *next = ticket->next;
        if (ticket->sender_session == session) ticket->sender_session = 0;
        if (ticket->receiver_session == session) ticket->receiver_session = 0;
        if ((!ticket->sender_session && ticket->info.state == UNIX_TICKET_PREPARING) || can_forget(ticket))
            free_ticket(broker, ticket);
        ticket = next;
    }
    struct unix_right_progress **cursor = &broker->rights_progress;
    while (*cursor) {
        struct unix_right_progress *item = *cursor;
        if (item->session != session) { cursor = &item->next; continue; }
        *cursor = item->next;
        free(item);
    }
}

void unix_rights_thread_died(struct unix_broker *broker, uint64_t owner)
{
    struct unix_ticket *ticket = broker->tickets;
    while (ticket) {
        struct unix_ticket *next = ticket->next;
        if (ticket->owner == owner) ticket->sender_acked = 1;
        if (ticket->owner == owner && (ticket->info.state == UNIX_TICKET_PREPARING ||
            ticket->info.state == UNIX_TICKET_CANCELLED)) free_ticket(broker, ticket);
        else if (can_forget(ticket)) free_ticket(broker, ticket);
        ticket = next;
    }
    struct unix_right_progress *item = broker->rights_progress;
    while (item) {
        struct unix_right_progress *next = item->next;
        if (item->owner == owner) {
            item->dead = 1;
            // A surviving thread may still need to publish hidden imports
            // from a completed receive, so preserve that receipt until ACK.
            if (!item->pending) retire_progress(broker, item);
        }
        item = next;
    }
}

void unix_rights_destroy(struct unix_broker *broker)
{
    while (broker->tickets) free_ticket(broker, broker->tickets);
    while (broker->rights_progress) {
        struct unix_right_progress *item = broker->rights_progress;
        broker->rights_progress = item->next;
        free(item);
    }
}

size_t unix_broker_ticket_count(const struct unix_broker *broker)
{
    return broker ? broker->ticket_count : 0;
}

static int prepare(struct unix_broker *broker, struct unix_session *session,
    uint64_t source_id, uint64_t route_id, uint64_t owner, uint64_t operation, unsigned total,
    const struct unix_credentials *claimed, uint64_t *out)
{
    if (!broker || !session || !out || !owner || owner == UNIX_TRANSPORT_RECOVERING ||
        !operation || total > UNIX_RIGHTS_MAX) return -EINVAL;
    for (struct unix_ticket *ticket = broker->tickets; ticket; ticket = ticket->next) {
        if (ticket->sender_session != session->id || ticket->owner != owner ||
            ticket->info.operation != operation) continue;
        if (ticket->info.source != source_id || ticket->route != route_id || ticket->expected != total) return -EINVAL;
        if (ticket->info.state == UNIX_TICKET_CANCELLED) return -ECANCELED;
        *out = ticket->info.ticket;
        return 0;
    }
    struct unix_socket *source = unix_broker_owned(broker, session, source_id);
    if (!source) return -EBADF;
    struct unix_route *route = NULL;
    if (source->type == UNIX_TRANSPORT_DGRAM) {
        route = unix_broker_route_lookup(broker, route_id);
        if (!route || route->source != source) return -ENOENT;
        if (route->destination->dgram_peer && route->destination->dgram_peer != source_id) return -EPERM;
    } else if (route_id || !source->connection || !source->peer) return -ENOTCONN;
    struct unix_right_progress *record = progress(broker, session, owner, 1);
    if (!record) return -ENOMEM;
    if (record->dead) return -ESRCH;
    if (operation <= record->sent) return -ESTALE;
    if (broker->ticket_count >= UNIX_TICKET_LIMIT) return -ENOBUFS;
    if (broker->next_id == UINT64_MAX) return -EOVERFLOW;
    if (claimed) {
        int status = unix_broker_check_credentials(session, claimed);
        if (status != 0) return status;
    }
    struct unix_ticket *ticket = calloc(1, sizeof(*ticket) + total * sizeof(ticket->refs[0]));
    if (!ticket) return -ENOMEM;
    ticket->info = (struct unix_rights_info){ .ticket = broker->next_id++,
        .source = source_id, .destination = route ? route->destination->id : source->peer->id,
        .generation = route ? route->id : source->connection->generation, .operation = operation,
        .state = UNIX_TICKET_PREPARING, .credentials = session->credentials };
    const struct unix_socket *destination = route ? route->destination : source->peer;
    ticket->info.credentials_present = claimed || !destination->owners ||
        ((source->options | destination->options) & UNIX_SOCKET_PASSCRED);
    if (claimed) {
        ticket->info.credentials.uid = claimed->uid;
        ticket->info.credentials.gid = claimed->gid;
    }
    ticket->sender_session = session->id;
    ticket->owner = owner;
    ticket->route = route_id;
    ticket->expected = total;
    ticket->next = broker->tickets;
    broker->tickets = ticket;
    broker->ticket_count++;
    record->sent = operation;
    *out = ticket->info.ticket;
    return 0;
}

int unix_broker_rights_prepare(struct unix_broker *broker, struct unix_session *session,
    uint64_t source, uint64_t owner, uint64_t operation, unsigned total,
    const struct unix_credentials *claimed, uint64_t *ticket)
{
    return prepare(broker, session, source, 0, owner, operation, total, claimed, ticket);
}

int unix_broker_rights_prepare_route(struct unix_broker *broker, struct unix_session *session,
    uint64_t source, uint64_t route, uint64_t owner, uint64_t operation, unsigned total,
    const struct unix_credentials *claimed, uint64_t *ticket)
{
    if (!route) return -EINVAL;
    return prepare(broker, session, source, route, owner, operation, total, claimed, ticket);
}

int unix_broker_rights_appended(struct unix_broker *broker, struct unix_session *session,
    uint64_t id, unsigned offset, struct unix_right_ref *refs, unsigned count, int *present)
{
    struct unix_ticket *ticket = find_ticket(broker, id);
    if (!ticket || ticket->sender_session != session->id) return -EPERM;
    if (ticket->info.state != UNIX_TICKET_PREPARING) return -EALREADY;
    if (!present || (!refs && count) || offset > ticket->expected ||
        count > ticket->expected - offset || offset > ticket->info.count) return -EINVAL;
    *present = offset < ticket->info.count;
    if (*present) {
        if (count > ticket->info.count - offset) return -EINVAL;
        if (count) memcpy(refs, ticket->refs + offset, count * sizeof(*refs));
    }
    return 0;
}

int unix_broker_rights_append(struct unix_broker *broker, struct unix_session *session,
    uint64_t id, unsigned offset, const struct unix_right_ref *refs, unsigned count)
{
    struct unix_ticket *ticket = find_ticket(broker, id);
    if (!ticket || ticket->sender_session != session->id) return -EPERM;
    if (ticket->info.state != UNIX_TICKET_PREPARING) return -EALREADY;
    if ((!refs && count) || offset > ticket->expected || count > ticket->expected - offset) return -EINVAL;
    if (offset < ticket->info.count) {
        if (count > ticket->info.count - offset) return -EINVAL;
        return memcmp(ticket->refs + offset, refs, count * sizeof(*refs)) == 0 ? 0 : -EINVAL;
    }
    if (offset != ticket->info.count) return -EINVAL;
    unsigned retained = 0;
    int status = 0;
    for (; retained < count; retained++) {
        const struct unix_right_ref ref = refs[retained];
        if (!!ref.socket == !!ref.external) { status = -EINVAL; break; }
        if (ref.socket) {
            if (!unix_broker_owned(broker, session, ref.socket)) { status = -EBADF; break; }
        } else {
            if (!broker->platform.external_retain || !broker->platform.external_release) {
                status = -EOPNOTSUPP; break;
            }
            status = broker->platform.external_retain(broker->platform.context, ref.external);
            if (status != 0) break;
        }
        ticket->refs[offset + retained] = ref;
    }
    if (status != 0) {
        for (unsigned i = 0; i < retained; i++)
            if (refs[i].external) broker->platform.external_release(broker->platform.context, refs[i].external);
        return status;
    }
    ticket->info.count += count;
    return 0;
}

int unix_broker_rights_commit(struct unix_broker *broker, struct unix_session *session,
    uint64_t id, struct unix_write *write)
{
    struct unix_ticket *ticket = find_ticket(broker, id);
    if (!ticket || ticket->sender_session != session->id) return -EPERM;
    if (ticket->info.state == UNIX_TICKET_QUEUED || ticket->info.state == UNIX_TICKET_DELIVERED ||
        ticket->info.state == UNIX_TICKET_DROPPED)
        return write && write->before == ticket->info.position && write->after == ticket->after &&
            write->length == ticket->info.length ? 0 : -EINVAL;
    if (ticket->info.state != UNIX_TICKET_PREPARING) return -ECANCELED;
    if (!write || write->owner != ticket->owner || ticket->info.count != ticket->expected) return -EINVAL;
    if (session->credentials.generation != ticket->info.credentials.generation) return -ESTALE;
    struct unix_socket *source = unix_broker_lookup(broker, ticket->info.source);
    struct unix_route *route = ticket->route ? unix_broker_route_lookup(broker, ticket->route) : NULL;
    if (!source) return -EPIPE;
    struct unix_direction *direction;
    if (ticket->route) {
        if (!route || route->source != source || route->destination->id != ticket->info.destination) return -EPIPE;
        direction = &route->direction;
    } else {
        if (!source->connection || !source->peer || source->peer->id != ticket->info.destination) return -EPIPE;
        if (source->connection->generation != ticket->info.generation) return -ESTALE;
        direction = &source->connection->directions[source->side];
    }
    if (write->length > UNIX_TRANSPORT_BYTES - sizeof(struct unix_record) ||
        (source->type == UNIX_TRANSPORT_STREAM && !write->length) ||
        write->before > UINT32_MAX || ((uint32_t)write->before & 31u)) return -EPROTO;
    const uint32_t charge = (write->length + sizeof(struct unix_record) + 31u) & ~31u;
    const uint64_t consumed = __atomic_load_n(&direction->rx->consumed, __ATOMIC_SEQ_CST);
    if (consumed & UNIX_TRANSPORT_CLOSED) return -EPIPE;
    const uint32_t used = (uint32_t)write->before - (uint32_t)consumed;
    if (((uint32_t)consumed & 31u) || used > UNIX_TRANSPORT_BYTES ||
        charge > UNIX_TRANSPORT_BYTES - used ||
        write->after != (uint32_t)((uint32_t)write->before + charge)) return -EPROTO;
    const struct unix_record *record = (const void *)(direction->tx->data +
        ((uint32_t)write->before & (UNIX_TRANSPORT_BYTES - 1u)));
    if (__atomic_load_n(&record->ticket, __ATOMIC_RELAXED) != id ||
        __atomic_load_n(&record->operation, __ATOMIC_RELAXED) != ticket->info.operation ||
        __atomic_load_n(&record->length, __ATOMIC_RELAXED) != write->length ||
        __atomic_load_n(&record->reserved, __ATOMIC_RELAXED) ||
        __atomic_load_n(&record->reserved1, __ATOMIC_RELAXED)) return -EPROTO;
    const int status = route ? unix_broker_dgram_enqueue(broker, route, write, id, ticket->info.operation,
        &ticket->info.credentials, &ticket->delivery) : unix_transport_write_commit(direction->tx, write);
    if (status != 0) return status;
    ticket->info.position = (uint32_t)write->before;
    ticket->info.length = write->length;
    ticket->after = write->after;
    ticket->info.state = UNIX_TICKET_QUEUED;
    signal_socket(broker, ticket->info.destination);
    signal_socket(broker, ticket->info.source);
    unix_broker_collect(broker);
    return 0;
}

int unix_broker_rights_cancel(struct unix_broker *broker, struct unix_session *session, uint64_t id)
{
    struct unix_ticket *ticket = find_ticket(broker, id);
    if (!ticket || ticket->sender_session != session->id) return -EPERM;
    if (ticket->info.state == UNIX_TICKET_CANCELLED) return 0;
    if (ticket->info.state != UNIX_TICKET_PREPARING) return -EALREADY;
    struct unix_socket *source = unix_broker_lookup(broker, ticket->info.source);
    struct unix_route *route = ticket->route ? unix_broker_route_lookup(broker, ticket->route) : NULL;
    if (source && (source->connection || route)) {
        // Cancel the local write reservation before cancelling its escrow.
        // A delayed control request must not unlock a later live operation
        // that happens to have the same thread owner token.
        const struct unix_tx *tx = route ? route->direction.tx : source->connection->directions[source->side].tx;
        if (__atomic_load_n(&tx->owner, __ATOMIC_SEQ_CST) == ticket->owner) return -EBUSY;
    }
    release_refs(broker, ticket);
    ticket->info.state = UNIX_TICKET_CANCELLED;
    signal_socket(broker, ticket->info.source);
    unix_broker_collect(broker);
    return 0;
}

static int validate_read(struct unix_broker *broker, struct unix_session *session,
    uint64_t socket, struct unix_ticket *ticket, const struct unix_read *read,
    struct unix_rx **rx)
{
    struct unix_socket *destination = unix_broker_owned(broker, session, socket);
    if (!destination || ticket->info.destination != socket) return -EPERM;
    if (ticket->info.state != UNIX_TICKET_QUEUED) return -ESTALE;
    if (ticket->route) {
        const struct unix_datagram *head = destination->datagram_head;
        if (!head || head->delivery.id != ticket->delivery || head->delivery.ticket != ticket->info.ticket ||
            head->delivery.route != ticket->route) return -ESTALE;
        *rx = head->route->direction.rx;
    } else {
        if (!destination->connection || destination->connection->generation != ticket->info.generation)
            return -ESTALE;
        *rx = destination->connection->directions[1u - destination->side].rx;
    }
    const uint32_t partial = read ? (uint32_t)(read->before >> 32) : 0;
    if (!read || !read->owner || read->owner == UNIX_TRANSPORT_RECOVERING ||
        (destination->type == UNIX_TRANSPORT_STREAM && !read->length) ||
        (uint32_t)read->before != ticket->info.position || read->operation != ticket->info.operation ||
        (destination->type != UNIX_TRANSPORT_STREAM && partial) || partial > ticket->info.length ||
        read->message_length != ticket->info.length || read->length > ticket->info.length - partial ||
        __atomic_load_n(&(*rx)->owner, __ATOMIC_SEQ_CST) != read->owner ||
        __atomic_load_n(&(*rx)->consumed, __ATOMIC_SEQ_CST) != read->before) return -ESTALE;
    const uint64_t after = destination->type != UNIX_TRANSPORT_STREAM || partial + read->length == ticket->info.length ?
        ticket->after : ticket->info.position | ((uint64_t)(partial + read->length) << 32);
    return read->after == after ? 0 : -EPROTO;
}

int unix_broker_rights_head(struct unix_broker *broker, struct unix_session *session, uint64_t socket,
    uint64_t id, const struct unix_read *read, struct unix_rights_info *info,
    struct unix_right_ref *refs, unsigned capacity)
{
    struct unix_ticket *ticket = find_ticket(broker, id);
    if (!info || (!refs && capacity)) return -EINVAL;
    if (!ticket) return -ESTALE;
    struct unix_rx *rx;
    const int status = validate_read(broker, session, socket, ticket, read, &rx);
    if (status != 0) return status;
    *info = ticket->info;
    const unsigned count = capacity < info->count ? capacity : info->count;
    if (count) memcpy(refs, ticket->refs, count * sizeof(*refs));
    return 0;
}

int unix_broker_rights_socket(struct unix_broker *broker, struct unix_session *session,
    uint64_t destination, uint64_t id, const struct unix_read *read, unsigned index,
    struct unix_attachment *out, struct unix_direction *tx, struct unix_direction *rx)
{
    struct unix_ticket *ticket = find_ticket(broker, id);
    if (!ticket) return -ESTALE;
    struct unix_rx *cursor;
    int status = validate_read(broker, session, destination, ticket, read, &cursor);
    if (status) return status;
    if (index >= ticket->info.count || !ticket->refs[index].socket) return -EINVAL;
    return unix_broker_socket_attachment(unix_broker_lookup(broker, ticket->refs[index].socket), out, tx, rx);
}

int unix_broker_rights_received(struct unix_broker *broker, struct unix_session *session, uint64_t socket,
    uint64_t id, uint64_t owner, uint64_t operation, unsigned take, int peek)
{
    struct unix_right_progress *record = progress(broker, session, owner, 0);
    return record && record->pending && operation == record->received &&
        record->receive_ticket == id && record->receive_socket == socket &&
        record->take == take && record->peek == (unsigned)peek ? 0 : -ESTALE;
}

int unix_broker_rights_consume(struct unix_broker *broker, struct unix_session *session, uint64_t socket,
    uint64_t id, uint64_t owner, uint64_t operation, struct unix_read *read, unsigned take, int peek)
{
    if (!operation || !owner || owner == UNIX_TRANSPORT_RECOVERING || !read || (peek != 0 && peek != 1)) return -EINVAL;
    struct unix_right_progress *record = progress(broker, session, owner, 1);
    if (!record) return -ENOMEM;
    if (record->pending && operation == record->received)
        return record->receive_ticket == id && record->receive_socket == socket &&
            record->take == take && record->peek == (unsigned)peek ? 0 : -ESTALE;
    if (record->dead) return -ESRCH;
    if (operation <= record->received) return -ESTALE;
    if (record->pending) return -EBUSY;
    struct unix_ticket *ticket = find_ticket(broker, id);
    if (read->owner != owner) return -EINVAL;
    if (!ticket) return -ESTALE;
    struct unix_rx *rx;
    int status = validate_read(broker, session, socket, ticket, read, &rx);
    if (status != 0) return status;
    if (take > ticket->info.count) return -EINVAL;
    unsigned imported = 0;
    for (; imported < take; imported++) {
        if (!ticket->refs[imported].socket) continue;
        status = unix_broker_import_socket(broker, session, ticket->refs[imported].socket);
        if (status != 0) break;
    }
    if (status == 0) {
        if (ticket->route) status = unix_broker_dgram_finish(broker,
            unix_broker_lookup(broker, socket), read, peek);
        else if (peek) unix_transport_read_cancel(rx, read);
        else status = unix_transport_read_commit(rx, read);
    }
    if (status != 0) {
        for (unsigned i = 0; i < imported; i++)
            if (ticket->refs[i].socket) (void)unix_broker_close(broker, session, ticket->refs[i].socket);
        return status;
    }
    record->received = operation;
    record->receive_ticket = id;
    record->receive_socket = socket;
    record->take = take;
    record->peek = (unsigned)peek;
    record->pending = 1;
    if (!peek) {
        ticket->receiver_session = session->id;
        ticket->receiver_owner = owner;
        ticket->receive_operation = operation;
        ticket->taken = take;
        ticket->receiver_acked = 0;
        ticket->info.state = read->after == ticket->after ? UNIX_TICKET_DELIVERED : UNIX_TICKET_QUEUED;
        release_refs(broker, ticket);
    }
    signal_socket(broker, ticket->info.source);
    signal_socket(broker, socket);
    unix_broker_collect(broker);
    return 0;
}

int unix_broker_rights_ack(struct unix_broker *broker, struct unix_session *session,
    uint64_t id, uint64_t owner, uint64_t operation, int receiving)
{
    if (!owner || !operation || (receiving != 0 && receiving != 1)) return -EINVAL;
    struct unix_right_progress *record = progress(broker, session, owner, 0);
    if (!record) return -ESTALE;
    struct unix_ticket *ticket = find_ticket(broker, id);
    if (receiving) {
        if (record->received != operation || record->receive_ticket != id) return -ESTALE;
        record->pending = 0;
        if (ticket && ticket->receiver_session == session->id &&
            ticket->receiver_owner == owner && ticket->receive_operation == operation)
            ticket->receiver_acked = 1;
    } else {
        if (!ticket) return operation <= record->sent ? 0 : -ESTALE;
        if (ticket->sender_session != session->id || ticket->owner != owner ||
            ticket->info.operation != operation) return -EPERM;
        if (ticket->info.state == UNIX_TICKET_PREPARING) return -EBUSY;
        ticket->sender_acked = 1;
    }
    if (ticket && can_forget(ticket)) free_ticket(broker, ticket);
    if (record->dead && !record->pending) retire_progress(broker, record);
    return 0;
}
