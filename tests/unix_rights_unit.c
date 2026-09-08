#include "rights.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fixture { unsigned directions; unsigned external[4]; unsigned notifications; };

static int direction_new(void *raw, uint64_t generation, uint32_t type, struct unix_direction *out)
{
    struct fixture *fixture = raw;
    out->tx = calloc(1, sizeof(*out->tx)); out->rx = calloc(1, sizeof(*out->rx));
    assert(out->tx && out->rx);
    fixture->directions++;
    return unix_transport_init(out->tx, out->rx, generation, type);
}
static void direction_free(void *raw, struct unix_direction *direction)
{
    assert(((struct fixture *)raw)->directions-- != 0);
    free(direction->tx); free(direction->rx);
}
static void signal_socket(void *raw, uint64_t id)
{
    assert(id);
    ((struct fixture *)raw)->notifications++;
}
static int external_retain(void *raw, uint64_t id)
{
    if (id >= 4) return -EBADF;
    struct fixture *fixture = raw;
    assert(fixture->external[id]);
    fixture->external[id]++;
    return 0;
}
static void external_release(void *raw, uint64_t id)
{
    struct fixture *fixture = raw;
    assert(id < 4 && fixture->external[id]);
    fixture->external[id]--;
}
static struct unix_session *session_new(struct unix_broker *broker, int pid)
{
    struct unix_session *session;
    const struct unix_credentials credentials = { .pid = pid, .generation = 1,
        .uid = 1000, .gid = 1000, .euid = 1000, .egid = 1000, .suid = 1000, .sgid = 1000 };
    assert(unix_broker_session_create(broker, &credentials, &session) == 0);
    return session;
}
static struct unix_direction attach_tx(struct unix_broker *broker, struct unix_session *session,
    uint64_t socket, uint64_t *generation)
{
    struct unix_direction tx, rx;
    struct unix_attachment attachment;
    assert(unix_broker_attach(broker, session, socket, &attachment, &tx, &rx) == 0);
    *generation = attachment.generation;
    return tx;
}
static uint64_t send_rights(struct unix_broker *broker, struct unix_session *session,
    uint64_t socket, uint64_t operation, const struct unix_right_ref *refs, unsigned count)
{
    uint64_t ticket, generation;
    assert(unix_broker_rights_prepare(broker, session, socket, 4, operation, count, NULL, &ticket) == 0);
    assert(unix_broker_rights_append(broker, session, ticket, 0, refs, count) == 0);
    struct unix_direction direction = attach_tx(broker, session, socket, &generation);
    struct unix_write write;
    assert(unix_transport_write_begin(direction.tx, direction.rx, generation, 4, 4,
        ticket, operation, &write) == 0);
    memcpy(write.spans[0].base, "data", 4);
    assert(unix_broker_rights_commit(broker, session, ticket, &write) == 0);
    assert(unix_broker_rights_commit(broker, session, ticket, &write) == 0);
    return ticket;
}

static void sender_death_prefix(struct unix_broker *broker, struct fixture *fixture)
{
    struct unix_session *sender = session_new(broker, 100), *receiver = session_new(broker, 101);
    uint64_t carrier[2], resource[2];
    assert(unix_broker_socketpair(broker, sender, UNIX_TRANSPORT_STREAM, 0, carrier) == 0);
    assert(unix_broker_socketpair(broker, sender, UNIX_TRANSPORT_STREAM, 0, resource) == 0);
    assert(unix_broker_retain(broker, sender, carrier[1], receiver) == 0);
    assert(unix_broker_close(broker, sender, carrier[1]) == 0);
    fixture->external[1] = 1;
    const struct unix_right_ref refs[3] = {
        { .socket = resource[0] }, { .external = 1 }, { .socket = resource[1] },
    };
    uint64_t generation;
    struct unix_direction incoming = attach_tx(broker, sender, carrier[0], &generation);
    uint64_t ticket = send_rights(broker, sender, carrier[0], 7, refs, 3);
    assert(fixture->external[1] == 2);
    assert(unix_broker_rights_cancel(broker, sender, ticket) == -EALREADY);
    unix_broker_thread_died(broker, 4); /* queued rights survive sender thread exit */
    unix_broker_session_destroy(broker, sender);
    external_release(fixture, 1); /* sender's original external reference */
    assert(fixture->external[1] == 1 && unix_broker_socket_count(broker) == 3);
    struct unix_read read;
    assert(unix_transport_read_begin(incoming.tx, incoming.rx, generation, 9, 2, &read) == 0);
    struct unix_rights_info info;
    struct unix_right_ref received;
    assert(unix_broker_rights_head(broker, receiver, carrier[0], ticket, &read, &info, &received, 1) == -EPERM);
    struct unix_read forged_read = read;
    forged_read.before += 64;
    assert(unix_broker_rights_head(broker, receiver, carrier[1], ticket, &forged_read, &info, &received, 1) == -ESTALE);
    assert(unix_broker_rights_head(broker, receiver, carrier[1], ticket, &read, &info, &received, 1) == 0);
    assert(info.count == 3 && info.credentials.pid == 100 && received.socket == resource[0]);
    struct unix_socket_diagnostic diagnostic;
    assert(unix_broker_diagnose(broker, receiver, resource[0], &diagnostic) == -EBADF); /* still hidden */
    assert(unix_broker_rights_consume(broker, receiver, carrier[1], ticket, 9, 10, &read, 1, 0) == 0);
    assert(fixture->external[1] == 0 && unix_broker_socket_count(broker) == 2);
    assert(unix_broker_rights_consume(broker, receiver, carrier[1], ticket, 9, 10, &read, 1, 0) == 0);
    assert(unix_broker_rights_consume(broker, receiver, carrier[1], ticket, 9, 11, &read, 1, 0) == -EBUSY);
    assert(unix_broker_rights_ack(broker, receiver, ticket, 9, 10, 1) == 0);
    assert(unix_broker_rights_consume(broker, receiver, carrier[1], ticket, 9, 10, &read, 1, 0) == -ESTALE);
    assert(unix_broker_diagnose(broker, receiver, resource[0], &diagnostic) == 0);
    assert(diagnostic.references == 1 && diagnostic.peer == 0);
    assert(unix_transport_read_begin(incoming.tx, incoming.rx, generation, 9, 4, &read) == 0);
    assert(read.length == 2 && read.ticket == ticket && memcmp(read.spans[0].base, "ta", 2) == 0);
    assert(unix_broker_rights_head(broker, receiver, carrier[1], ticket, &read, &info, &received, 1) == 0);
    assert(info.count == 0 && info.credentials.pid == 100); /* FD once, identity for all bytes */
    assert(unix_broker_rights_consume(broker, receiver, carrier[1], ticket, 9, 11, &read, 0, 0) == 0);
    assert(unix_broker_rights_ack(broker, receiver, ticket, 9, 11, 1) == 0);
    assert(unix_transport_read_begin(incoming.tx, incoming.rx, generation, 9, 4, &read) == 0 && read.eof);
    unix_broker_session_destroy(broker, receiver);
    assert(unix_broker_socket_count(broker) == 0 && unix_broker_ticket_count(broker) == 0);
}

static void cyclic_queues(struct unix_broker *broker)
{
    struct unix_session *session = session_new(broker, 200);
    uint64_t a[2], b[2];
    assert(unix_broker_socketpair(broker, session, UNIX_TRANSPORT_STREAM, 0, a) == 0);
    assert(unix_broker_socketpair(broker, session, UNIX_TRANSPORT_STREAM, 0, b) == 0);
    const struct unix_right_ref aright = { .socket = b[1] }, bright = { .socket = a[1] };
    (void)send_rights(broker, session, a[0], 1, &aright, 1);
    (void)send_rights(broker, session, b[0], 2, &bright, 1);
    assert(unix_broker_close(broker, session, a[0]) == 0);
    assert(unix_broker_close(broker, session, b[0]) == 0);
    assert(unix_broker_close(broker, session, a[1]) == 0);
    assert(unix_broker_socket_count(broker) == 2); /* reachable through B's queued FD */
    assert(unix_broker_close(broker, session, b[1]) == 0);
    assert(unix_broker_socket_count(broker) == 0); /* no external root: reclaim cycle */
    unix_broker_session_destroy(broker, session);
    assert(unix_broker_ticket_count(broker) == 0);
}

static void preparation_failure(struct unix_broker *broker, struct fixture *fixture)
{
    struct unix_session *sender = session_new(broker, 300), *foreign = session_new(broker, 301);
    uint64_t pair[2], other, ticket, duplicate, generation;
    assert(unix_broker_socketpair(broker, sender, UNIX_TRANSPORT_STREAM, 0, pair) == 0);
    assert(unix_broker_socket(broker, foreign, UNIX_TRANSPORT_STREAM, 0, &other) == 0);
    fixture->external[1] = 1;
    const struct unix_credentials forged_credentials = { .pid = 301, .generation = 1,
        .uid = 1000, .gid = 1000 };
    assert(unix_broker_rights_prepare(broker, sender, pair[0], 4, 1, 2, &forged_credentials, &ticket) == -EPERM);
    assert(unix_broker_rights_prepare(broker, sender, pair[0], 4, 1, 2, NULL, &ticket) == 0);
    assert(unix_broker_rights_prepare(broker, sender, pair[0], 4, 1, 2, NULL, &duplicate) == 0 && ticket == duplicate);
    assert(unix_broker_rights_prepare(broker, sender, pair[1], 4, 1, 2, NULL, &duplicate) == -EINVAL);
    struct unix_right_ref refs[2] = { { .external = 1 }, { .socket = other } };
    assert(unix_broker_rights_append(broker, sender, ticket, 0, refs, 2) == -EBADF);
    assert(fixture->external[1] == 1); /* rollback first retained item */
    refs[1].socket = pair[0];
    assert(unix_broker_rights_append(broker, sender, ticket, 1, refs, 1) == -EINVAL);
    assert(unix_broker_rights_append(broker, sender, ticket, 0, refs, 2) == 0);
    assert(unix_broker_rights_append(broker, sender, ticket, 0, refs, 2) == 0);
    assert(fixture->external[1] == 2);
    struct unix_direction direction = attach_tx(broker, sender, pair[0], &generation);
    struct unix_write write;
    assert(unix_transport_write_begin(direction.tx, direction.rx, generation, 4, 1, ticket, 1, &write) == 0);
    struct unix_write forged = write;
    forged.after += 32;
    assert(unix_broker_rights_commit(broker, sender, ticket, &forged) == -EPROTO);
    assert(direction.tx->published == 0);
    unix_broker_thread_died(broker, 4);
    assert(direction.tx->published == 0 && direction.tx->owner == 0);
    assert(fixture->external[1] == 1 && !unix_broker_ticket_count(broker));
    assert(unix_broker_rights_prepare(broker, sender, pair[0], 5, 2, 0, NULL, &ticket) == 0);
    assert(unix_transport_write_begin(direction.tx, direction.rx, generation, 5, 1, ticket, 2, &write) == 0);
    assert(unix_broker_close(broker, sender, pair[1]) == 0);
    assert(unix_broker_rights_commit(broker, sender, ticket, &write) == -EPIPE);
    assert(unix_broker_rights_cancel(broker, sender, ticket) == -EBUSY);
    unix_transport_write_cancel(direction.tx, &write);
    assert(unix_broker_rights_cancel(broker, sender, ticket) == 0);
    assert(direction.tx->owner == 0 && (uint32_t)direction.tx->published == 0);
    assert(unix_broker_rights_cancel(broker, sender, ticket) == 0);
    unix_broker_session_destroy(broker, sender);
    unix_broker_session_destroy(broker, foreign);
    external_release(fixture, 1);
    assert(!unix_broker_socket_count(broker) && !unix_broker_ticket_count(broker));
}

static void maximum_bundle(struct unix_broker *broker)
{
    struct unix_session *sender = session_new(broker, 400), *receiver = session_new(broker, 401);
    uint64_t pair[2], resource, ticket, generation;
    assert(unix_broker_socketpair(broker, sender, UNIX_TRANSPORT_SEQPACKET, 0, pair) == 0);
    assert(unix_broker_retain(broker, sender, pair[1], receiver) == 0);
    assert(unix_broker_socket(broker, sender, UNIX_TRANSPORT_STREAM, 0, &resource) == 0);
    assert(unix_broker_rights_prepare(broker, sender, pair[0], 4, 1, UNIX_RIGHTS_MAX + 1, NULL, &ticket) == -EINVAL);
    assert(unix_broker_rights_prepare(broker, sender, pair[0], 4, 1, UNIX_RIGHTS_MAX, NULL, &ticket) == 0);
    struct unix_right_ref refs[UNIX_RIGHTS_MAX];
    for (unsigned i = 0; i < UNIX_RIGHTS_MAX; i++) refs[i] = (struct unix_right_ref){ .socket = resource };
    for (unsigned i = 0; i < UNIX_RIGHTS_MAX; i += 16) {
        const unsigned count = UNIX_RIGHTS_MAX - i < 16 ? UNIX_RIGHTS_MAX - i : 16;
        assert(unix_broker_rights_append(broker, sender, ticket, i, refs + i, count) == 0);
    }
    struct unix_direction direction = attach_tx(broker, sender, pair[0], &generation);
    struct unix_write write;
    assert(unix_transport_write_begin(direction.tx, direction.rx, generation, 4, 0, ticket, 1, &write) == 0);
    assert(unix_broker_rights_commit(broker, sender, ticket, &write) == 0);
    unix_broker_session_destroy(broker, sender);
    struct unix_read read;
    assert(unix_transport_read_begin(direction.tx, direction.rx, generation, 9, 0, &read) == 0 && !read.eof);
    assert(unix_broker_rights_consume(broker, receiver, pair[1], ticket, 9, 2, &read, UNIX_RIGHTS_MAX, 0) == 0);
    struct unix_socket_diagnostic diagnostic;
    assert(unix_broker_diagnose(broker, receiver, resource, &diagnostic) == 0);
    assert(diagnostic.references == UNIX_RIGHTS_MAX && diagnostic.owners == 1);
    unix_broker_session_destroy(broker, receiver);
    assert(!unix_broker_socket_count(broker) && !unix_broker_ticket_count(broker));
}

static void peek_ack_and_reuse(struct unix_broker *broker)
{
    struct unix_session *session = session_new(broker, 500);
    uint64_t pair[2], resource, generation;
    assert(unix_broker_socketpair(broker, session, UNIX_TRANSPORT_STREAM, 0, pair) == 0);
    assert(unix_broker_socket(broker, session, UNIX_TRANSPORT_STREAM, 0, &resource) == 0);
    struct unix_direction direction = attach_tx(broker, session, pair[0], &generation);
    const struct unix_right_ref ref = { .socket = resource };
    uint64_t ticket = send_rights(broker, session, pair[0], 1, &ref, 1);
    assert(unix_broker_rights_ack(broker, session, ticket, 4, 1, 0) == 0);
    struct unix_read read;
    assert(unix_transport_read_begin(direction.tx, direction.rx, generation, 9, 4, &read) == 0);
    const uint64_t original_position = direction.rx->consumed;
    assert(unix_broker_rights_consume(broker, session, pair[1], ticket, 9, 1, &read, 1, 1) == 0);
    assert(direction.rx->consumed == original_position && direction.rx->owner == 0);
    assert(unix_broker_rights_consume(broker, session, pair[1], ticket, 9, 1, &read, 1, 1) == 0);
    struct unix_socket_diagnostic diagnostic;
    assert(unix_broker_diagnose(broker, session, resource, &diagnostic) == 0 && diagnostic.references == 2);
    assert(unix_broker_rights_ack(broker, session, ticket, 9, 1, 1) == 0);
    assert(unix_broker_rights_consume(broker, session, pair[1], ticket, 9, 1, &read, 1, 1) == -ESTALE);
    assert(unix_transport_read_begin(direction.tx, direction.rx, generation, 9, 4, &read) == 0);
    unix_broker_thread_died(broker, 9); /* reader died before consume: packet stays */
    assert(direction.rx->consumed == original_position && direction.rx->owner == 0);
    assert(unix_transport_read_begin(direction.tx, direction.rx, generation, 10, 4, &read) == 0);
    assert(unix_broker_rights_consume(broker, session, pair[1], ticket, 10, 1, &read, 0, 0) == 0);
    unix_broker_thread_died(broker, 10); /* after consume: same-process recovery can query result */
    assert(unix_broker_rights_consume(broker, session, pair[1], ticket, 10, 1, &read, 0, 0) == 0);
    assert(unix_broker_rights_ack(broker, session, ticket, 10, 1, 1) == 0);
    assert(unix_broker_ticket_count(broker) == 0);
    uint64_t stale;
    assert(unix_broker_rights_prepare(broker, session, pair[0], 4, 1, 1, NULL, &stale) == -ESTALE);
    assert(unix_broker_diagnose(broker, session, resource, &diagnostic) == 0 && diagnostic.references == 2);
    /* ACK must allow unbounded sequential traffic with bounded receipt storage. */
    for (unsigned i = 0; i < 1500; i++) {
        ticket = send_rights(broker, session, pair[0], i + 2, NULL, 0);
        assert(unix_broker_rights_ack(broker, session, ticket, 4, i + 2, 0) == 0);
        assert(unix_transport_read_begin(direction.tx, direction.rx, generation, 11, 4, &read) == 0);
        assert(unix_broker_rights_consume(broker, session, pair[1], ticket, 11, i + 1, &read, 0, 0) == 0);
        assert(unix_broker_rights_ack(broker, session, ticket, 11, i + 1, 1) == 0);
        assert(unix_broker_ticket_count(broker) == 0);
    }
    unix_broker_session_destroy(broker, session);
    assert(!unix_broker_socket_count(broker));
}

static void datagram_rights(struct unix_broker *broker)
{
    struct unix_session *a = session_new(broker, 600), *b = session_new(broker, 601),
        *c = session_new(broker, 602);
    uint64_t source_a, destination, source_c, resource, ab, cb, abgen, cbgen;
    assert(unix_broker_socket(broker, a, UNIX_TRANSPORT_DGRAM, 0, &source_a) == 0);
    assert(unix_broker_socket(broker, b, UNIX_TRANSPORT_DGRAM, 0, &destination) == 0);
    assert(unix_broker_socket(broker, c, UNIX_TRANSPORT_DGRAM, 0, &source_c) == 0);
    assert(unix_broker_socket(broker, a, UNIX_TRANSPORT_STREAM, 0, &resource) == 0);
    const struct unix_address address = { .kind = UNIX_ADDRESS_ABSTRACT,
        .length = 1, .bytes = {'d'} };
    assert(unix_broker_bind(broker, b, destination, &address) == 0);
    struct unix_direction adir, cdir;
    assert(unix_broker_dgram_route(broker, a, source_a, &address, &ab, &abgen, &adir) == 0);
    assert(unix_broker_dgram_route(broker, c, source_c, &address, &cb, &cbgen, &cdir) == 0);
    uint64_t ticket, ignored;
    assert(unix_broker_rights_prepare_route(broker, c, source_c, ab, 6, 1, 1, NULL, &ignored) == -ENOENT);
    assert(unix_broker_rights_prepare_route(broker, a, source_a, ab, 4, 1, 1, NULL, &ticket) == 0);
    const struct unix_right_ref right = { .socket = resource };
    assert(unix_broker_rights_append(broker, a, ticket, 0, &right, 1) == 0);
    struct unix_write full, carrying;
    assert(unix_transport_write_begin(cdir.tx, cdir.rx, cbgen, 6,
        UNIX_TRANSPORT_BYTES - sizeof(struct unix_record), 0, 1, &full) == 0);
    uint64_t full_delivery;
    assert(unix_broker_dgram_commit(broker, c, cb, &full, &full_delivery) == 0);
    assert(unix_transport_write_begin(adir.tx, adir.rx, abgen, 4, 4, ticket, 1, &carrying) == 0);
    memcpy(carrying.spans[0].base, "file", 4);
    assert(unix_broker_dgram_commit(broker, a, ab, &carrying, &ignored) != 0);
    assert(unix_broker_rights_commit(broker, a, ticket, &carrying) == -EAGAIN);
    assert(adir.tx->published == 0 && adir.tx->owner == 4);
    struct unix_delivery delivery;
    struct unix_direction incoming;
    struct unix_read read;
    assert(unix_broker_dgram_head(broker, b, destination, &delivery, &incoming) == 0);
    assert(delivery.id == full_delivery && !delivery.ticket);
    assert(unix_transport_packet_begin(incoming.tx, incoming.rx, delivery.generation, 9,
        delivery.position, delivery.length, 0, delivery.operation, 0, &read) == 0);
    assert(unix_broker_dgram_consume(broker, b, destination, delivery.id, &read, 0) == 0);

    uint64_t ordinary[2];
    for (unsigned i = 0; i < 2; i++) {
        struct unix_write write;
        assert(unix_transport_write_begin(cdir.tx, cdir.rx, cbgen, 6, 1, 0, i + 2, &write) == 0);
        *(char *)write.spans[0].base = (char)('a' + i);
        assert(unix_broker_dgram_commit(broker, c, cb, &write, &ordinary[i]) == 0);
        if (!i) assert(unix_broker_rights_commit(broker, a, ticket, &carrying) == 0);
    }
    assert(unix_transport_packet_begin(adir.tx, adir.rx, abgen, 9, 0, 4, ticket, 1, 2, &read) == 0);
    struct unix_rights_info info;
    assert(unix_broker_rights_head(broker, b, destination, ticket, &read, &info, NULL, 0) == -ESTALE);
    unix_transport_read_cancel(adir.rx, &read); /* earlier sender still at queue head */
    unix_broker_session_destroy(broker, a);

    assert(unix_broker_dgram_head(broker, b, destination, &delivery, &incoming) == 0);
    assert(delivery.id == ordinary[0] && !delivery.ticket);
    assert(unix_transport_packet_begin(incoming.tx, incoming.rx, delivery.generation, 9,
        delivery.position, delivery.length, 0, delivery.operation, 1, &read) == 0);
    assert(unix_broker_dgram_consume(broker, b, destination, delivery.id, &read, 0) == 0);
    assert(unix_broker_dgram_head(broker, b, destination, &delivery, &incoming) == 0);
    assert(delivery.ticket == ticket && delivery.credentials.pid == 600 && delivery.length == 4);
    /* Even the complete sender-writable header cannot alter the FD binding
     * or block a later sender. The delivery record supplies all metadata. */
    incoming.tx->magic = incoming.tx->generation = incoming.tx->published = 0;
    struct unix_record *record = (void *)incoming.tx->data;
    record->length = UINT32_MAX; record->ticket = 0; record->operation = UINT64_MAX;
    assert(unix_transport_packet_begin(incoming.tx, incoming.rx, delivery.generation, 9,
        delivery.position, delivery.length, delivery.ticket, delivery.operation, 2, &read) == 0);
    assert(read.truncated && memcmp(read.spans[0].base, "fi", 2) == 0);
    assert(unix_broker_dgram_consume(broker, b, destination, delivery.id, &read, 0) == -EPROTO);
    assert(unix_broker_rights_head(broker, b, destination, ticket, &read, &info, NULL, 0) == 0);
    assert(info.count == 1 && info.credentials.pid == 600 && info.generation == abgen);
    assert(unix_broker_rights_consume(broker, b, destination, ticket, 9, 1, &read, 1, 1) == 0);
    assert(unix_broker_rights_ack(broker, b, ticket, 9, 1, 1) == 0);
    struct unix_socket_diagnostic diagnostic;
    assert(unix_broker_diagnose(broker, b, destination, &diagnostic) == 0 && diagnostic.datagram_count == 2);
    assert(unix_transport_packet_begin(incoming.tx, incoming.rx, delivery.generation, 9,
        delivery.position, delivery.length, delivery.ticket, delivery.operation, 2, &read) == 0);
    assert(unix_broker_rights_consume(broker, b, destination, ticket, 9, 2, &read, 0, 0) == 0);
    assert(unix_broker_rights_consume(broker, b, destination, ticket, 9, 2, &read, 0, 0) == 0);
    assert(unix_broker_rights_ack(broker, b, ticket, 9, 2, 1) == 0);
    assert(unix_broker_ticket_count(broker) == 0);
    assert(unix_broker_diagnose(broker, b, resource, &diagnostic) == 0 && diagnostic.references == 1);
    assert(unix_broker_dgram_head(broker, b, destination, &delivery, &incoming) == 0);
    assert(delivery.id == ordinary[1] && delivery.credentials.pid == 602 && !delivery.ticket);
    assert(unix_transport_packet_begin(incoming.tx, incoming.rx, delivery.generation, 9,
        delivery.position, delivery.length, 0, delivery.operation, 1, &read) == 0);
    assert(unix_broker_dgram_consume(broker, b, destination, delivery.id, &read, 0) == 0);
    unix_broker_session_destroy(broker, b);
    unix_broker_session_destroy(broker, c);
    assert(!unix_broker_socket_count(broker) && !unix_broker_ticket_count(broker));
}

static void datagram_drop_rights(struct unix_broker *broker, struct fixture *fixture)
{
    struct unix_session *s = session_new(broker, 701);
    uint64_t pair[2], resource, ticket, route, generation;
    assert(unix_broker_socketpair(broker, s, UNIX_TRANSPORT_DGRAM, 0, pair) == 0);
    assert(unix_broker_socket(broker, s, UNIX_TRANSPORT_STREAM, 0, &resource) == 0);
    struct unix_direction direction, incoming;
    assert(unix_broker_dgram_route(broker, s, pair[0], NULL, &route, &generation, &direction) == 0);
    fixture->external[1] = 1;
    const struct unix_right_ref refs[2] = {{.socket = resource}, {.external = 1}};
    assert(unix_broker_rights_prepare_route(broker, s, pair[0], route, 4, 1, 2, NULL, &ticket) == 0);
    assert(unix_broker_rights_append(broker, s, ticket, 0, refs, 2) == 0);
    assert(unix_broker_close(broker, s, resource) == 0);
    struct unix_write write;
    assert(unix_transport_write_begin(direction.tx, direction.rx, generation, 4, 1, ticket, 1, &write) == 0);
    assert(unix_broker_rights_commit(broker, s, ticket, &write) == 0);
    struct unix_delivery delivery;
    struct unix_read read;
    struct unix_rights_info info;
    assert(unix_broker_dgram_head(broker, s, pair[1], &delivery, &incoming) == 0);
    assert(unix_transport_packet_begin(incoming.tx, incoming.rx, generation, 9,
        delivery.position, delivery.length, ticket, 1, 1, &read) == 0);
    assert(unix_broker_rights_head(broker, s, pair[1], ticket, &read, &info, NULL, 0) == 0);
    struct unix_address disconnect = {0};
    assert(unix_broker_connect(broker, s, pair[1], &disconnect) == 0);
    assert(fixture->external[1] == 1 && unix_broker_socket_count(broker) == 2);
    assert(unix_broker_rights_consume(broker, s, pair[1], ticket, 9, 1, &read, 2, 0) == -ESTALE);
    unix_transport_read_cancel(incoming.rx, &read);
    assert(unix_broker_rights_commit(broker, s, ticket, &write) == 0); /* send already succeeded */
    assert(unix_broker_rights_ack(broker, s, ticket, 4, 1, 0) == 0);
    assert(!unix_broker_ticket_count(broker));
    external_release(fixture, 1);
    unix_broker_session_destroy(broker, s);
    assert(!unix_broker_socket_count(broker));
}

int main(void)
{
    struct fixture fixture = {0};
    const struct unix_broker_platform platform = { .context = &fixture,
        .direction_create = direction_new, .direction_destroy = direction_free, .notify = signal_socket,
        .external_retain = external_retain, .external_release = external_release };
    struct unix_broker *broker = unix_broker_create(&platform);
    assert(broker);
    sender_death_prefix(broker, &fixture);
    cyclic_queues(broker);
    preparation_failure(broker, &fixture);
    maximum_bundle(broker);
    peek_ack_and_reuse(broker);
    datagram_rights(broker);
    datagram_drop_rights(broker, &fixture);
    unix_broker_destroy(broker);
    assert(!fixture.directions && !fixture.external[1] && fixture.notifications);
    puts("unix rights: escrow, sender/receiver death, prefix discard, partial stream, retries, cyclic GC, rollback, 253 references, zero packet, PEEK, ACK/reuse, datagram ordering/pressure/header isolation passed");
}
