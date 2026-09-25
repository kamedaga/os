#include "broker.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fixture {
    unsigned directions;
    unsigned notifications;
    int fail_after;
};

static int direction_create(void *context, uint64_t generation, uint32_t type,
    struct unix_direction *direction)
{
    struct fixture *fixture = context;
    if (fixture->fail_after == 0) return -EMFILE;
    if (fixture->fail_after > 0) fixture->fail_after--;
    direction->tx = calloc(1, sizeof(*direction->tx));
    direction->rx = calloc(1, sizeof(*direction->rx));
    assert(direction->tx && direction->rx);
    fixture->directions++;
    return unix_transport_init(direction->tx, direction->rx, generation, type);
}

static void direction_destroy(void *context, struct unix_direction *direction)
{
    struct fixture *fixture = context;
    assert(fixture->directions != 0);
    fixture->directions--;
    free(direction->tx);
    free(direction->rx);
}

static void notify(void *context, uint64_t socket)
{
    assert(socket != 0);
    ((struct fixture *)context)->notifications++;
}

static struct unix_session *session(struct unix_broker *broker, int pid)
{
    struct unix_credentials credentials = { .generation = 1, .pid = pid,
        .uid = 1000, .gid = 1000, .euid = 1000, .egid = 1000,
        .suid = 1000, .sgid = 1000 };
    struct unix_session *out = NULL;
    assert(unix_broker_session_create(broker, &credentials, &out) == 0);
    return out;
}

static uint64_t new_socket(struct unix_broker *broker, struct unix_session *owner)
{
    uint64_t id;
    assert(unix_broker_socket(broker, owner, UNIX_TRANSPORT_STREAM, 0, &id) == 0);
    return id;
}

static void direct_and_lifetime(struct unix_broker *broker, struct fixture *fixture)
{
    struct unix_session *parent = session(broker, 100);
    struct unix_session *child = session(broker, 101);
    uint64_t pair[2];
    assert(unix_broker_socketpair(broker, parent, UNIX_TRANSPORT_STREAM, 0, pair) == 0);
    assert(fixture->directions == 2);
    assert(unix_broker_close(broker, child, pair[0]) == -EBADF);
    struct unix_socket_diagnostic diagnostic;
    assert(unix_broker_diagnose(broker, child, pair[0], &diagnostic) == -EBADF);
    assert(unix_broker_retain(broker, parent, pair[0], child) == 0);
    assert(unix_broker_retain(broker, parent, pair[1], child) == 0);
    struct unix_attachment a, b;
    struct unix_direction atx, arx, btx, brx;
    assert(unix_broker_attach(broker, parent, pair[0], &a, &atx, &arx) == 0);
    assert(unix_broker_attach(broker, child, pair[1], &b, &btx, &brx) == 0);
    assert(atx.tx == brx.tx && arx.rx == btx.rx);
    assert(a.generation == b.generation);
    assert(unix_broker_set_flags(broker, parent, pair[0], 0x800) == 0);
    assert(atx.tx->flags == 0x800);
    assert(unix_broker_diagnose(broker, parent, pair[0], &diagnostic) == 0);
    assert(diagnostic.socket == pair[0] && diagnostic.peer == pair[1]);
    assert(diagnostic.session == unix_broker_session_id(parent));
    assert(diagnostic.owners == 2 && diagnostic.references == 2);
    assert(diagnostic.flags == 0x800 && diagnostic.outgoing.generation == a.generation);
    unix_broker_session_destroy(broker, parent);
    assert(unix_broker_socket_count(broker) == 2 && fixture->directions == 2);
    struct unix_write write;
    assert(unix_transport_write_begin(atx.tx, atx.rx, a.generation, 3, 4, 0, 0, &write) == 0);
    assert(unix_broker_diagnose(broker, child, pair[0], &diagnostic) == 0);
    assert(diagnostic.owners == 1 && diagnostic.references == 1);
    assert(diagnostic.outgoing.writer == 3 && diagnostic.outgoing.published == 0);
    memcpy(write.spans[0].base, "data", 4);
    assert(unix_transport_write_commit(atx.tx, &write) == 0);
    assert(unix_broker_close(broker, child, pair[0]) == 0);
    struct unix_read read;
    assert(unix_transport_read_begin(brx.tx, brx.rx, b.generation, 4, 4, &read) == 0);
    assert(unix_broker_diagnose(broker, child, pair[1], &diagnostic) == 0);
    assert(diagnostic.peer == 0 && diagnostic.incoming.reader == 4);
    assert((diagnostic.incoming.published & UNIX_TRANSPORT_CLOSED) != 0);
    assert(diagnostic.incoming.consumed == 0);
    assert(read.length == 4 && memcmp(read.spans[0].base, "data", 4) == 0);
    assert(unix_transport_read_commit(brx.rx, &read) == 0);
    assert(unix_transport_read_begin(brx.tx, brx.rx, b.generation, 4, 4, &read) == 0 && read.eof);
    unix_broker_session_destroy(broker, child);
    assert(unix_broker_socket_count(broker) == 0 && fixture->directions == 0);
}

static void names_backlog_credentials(struct unix_broker *broker, struct fixture *fixture)
{
    struct unix_session *server = session(broker, 200);
    struct unix_session *client = session(broker, 201);
    uint64_t listener = new_socket(broker, server);
    uint64_t other = new_socket(broker, server);
    const struct unix_address name = { .kind = UNIX_ADDRESS_ABSTRACT,
        .length = 3, .bytes = {'a', 0, 'b'} };
    struct unix_address different = name;
    different.length = 1;
    assert(unix_broker_bind(broker, server, listener, &name) == 0);
    assert(unix_broker_bind(broker, server, other, &name) == -EADDRINUSE);
    assert(unix_broker_bind(broker, server, other, &different) == 0);
    assert(unix_broker_listen(broker, server, listener, 1) == 0);
    /* SO_PEERCRED uses the listen-time snapshot, not credentials at accept. */
    struct unix_credentials changed = { .generation = 1, .pid = 200,
        .uid = 1001, .gid = 1001, .euid = 1002, .egid = 1002,
        .suid = 1003, .sgid = 1003 };
    assert(unix_broker_session_credentials(server, &changed) == 0);
    struct unix_credentials claimed = changed;
    claimed.uid = 1003;
    assert(unix_broker_check_credentials(server, &claimed) == 0);
    claimed.uid = 0;
    assert(unix_broker_check_credentials(server, &claimed) == -EPERM);
    claimed = changed;
    claimed.pid = 201;
    assert(unix_broker_check_credentials(server, &claimed) == -EPERM);
    claimed = changed;
    claimed.generation = 2;
    assert(unix_broker_check_credentials(server, &claimed) == -ESTALE);
    uint64_t first = new_socket(broker, client);
    uint64_t second = new_socket(broker, client);
    assert(unix_broker_connect(broker, client, first, &name) == 0);
    assert(unix_broker_connect(broker, client, second, &name) == -EAGAIN);
    struct unix_socket_diagnostic diagnostic;
    assert(unix_broker_diagnose(broker, server, listener, &diagnostic) == 0);
    assert(diagnostic.listening && diagnostic.bound);
    assert(diagnostic.pending == 1 && diagnostic.backlog == 1);
    struct unix_attachment attachment;
    struct unix_direction tx, rx;
    assert(unix_broker_attach(broker, client, first, &attachment, &tx, &rx) == 0);
    assert(attachment.peer_credentials.uid == 1000);
    uint64_t accepted;
    assert(unix_broker_accept(broker, client, listener, &accepted) == -EBADF);
    assert(unix_broker_accept(broker, server, listener, &accepted) == 0);
    assert(unix_broker_connect(broker, client, second, &name) == 0);
    assert(unix_broker_close(broker, server, listener) == 0); /* pending child dies */
    uint64_t replacement = new_socket(broker, server);
    /* An accepted socket's stored local name must not reserve the name. */
    assert(unix_broker_bind(broker, server, replacement, &name) == 0);
    assert(unix_broker_attach(broker, client, second, &attachment, &tx, &rx) == 0);
    assert(attachment.peer == 0);
    struct unix_write write;
    assert(unix_transport_write_begin(tx.tx, tx.rx, attachment.generation, 7,
        1, 0, 0, &write) == -EPIPE);
    unix_broker_session_destroy(broker, server);
    unix_broker_session_destroy(broker, client);
    assert(unix_broker_socket_count(broker) == 0 && fixture->directions == 0);
}

static void allocation_rollback(struct unix_broker *broker, struct fixture *fixture)
{
    struct unix_session *owner = session(broker, 300);
    for (int fail_after = 0; fail_after < 2; fail_after++) {
        fixture->fail_after = fail_after;
        uint64_t pair[2];
        assert(unix_broker_socketpair(broker, owner, UNIX_TRANSPORT_STREAM, 0, pair) == -EMFILE);
        assert(pair[0] == 0 && pair[1] == 0);
        assert(fixture->directions == 0 && unix_broker_socket_count(broker) == 0);
    }
    fixture->fail_after = -1;
    unix_broker_session_destroy(broker, owner);
}

static uint64_t datagram_socket(struct unix_broker *broker, struct unix_session *owner, char name)
{
    uint64_t id;
    assert(unix_broker_socket(broker, owner, UNIX_TRANSPORT_DGRAM, 0, &id) == 0);
    const struct unix_address address = { .kind = UNIX_ADDRESS_ABSTRACT,
        .length = 1, .bytes = {(uint8_t)name} };
    assert(unix_broker_bind(broker, owner, id, &address) == 0);
    return id;
}

static uint64_t datagram_send(struct unix_broker *broker, struct unix_session *sender,
    uint64_t route, uint64_t generation, const struct unix_direction *direction, const char *data)
{
    struct unix_write write;
    assert(unix_transport_write_begin(direction->tx, direction->rx, generation,
        11, 3, 0, 79, &write) == 0);
    memcpy(write.spans[0].base, data, 3);
    uint64_t delivery;
    assert(unix_broker_dgram_commit(broker, sender, route, &write, &delivery) == 0);
    return delivery;
}

static void datagram_isolation_and_order(struct unix_broker *broker, struct fixture *fixture)
{
    struct unix_session *a = session(broker, 401), *b = session(broker, 402);
    struct unix_session *c = session(broker, 403), *d = session(broker, 404);
    const uint64_t aid = datagram_socket(broker, a, 'a');
    const uint64_t bid = datagram_socket(broker, b, 'b');
    const uint64_t cid = datagram_socket(broker, c, 'c');
    (void)datagram_socket(broker, d, 'd');
    struct unix_address destination = { .kind = UNIX_ADDRESS_ABSTRACT,
        .length = 1, .bytes = {'b'} };
    uint64_t ab, abgen, cb, cbgen, ad, adgen;
    struct unix_direction abdir, cbdir, addir;
    assert(unix_broker_dgram_route(broker, a, aid, &destination, &ab, &abgen, &abdir) == 0);
    assert(unix_broker_dgram_route(broker, c, cid, &destination, &cb, &cbgen, &cbdir) == 0);
    destination.bytes[0] = 'd';
    assert(unix_broker_dgram_route(broker, a, aid, &destination, &ad, &adgen, &addir) == 0);
    assert(abdir.tx != cbdir.tx && abdir.tx != addir.tx && cbdir.tx != addir.tx);
    uint64_t deliveries[3];
    deliveries[0] = datagram_send(broker, a, ab, abgen, &abdir, "abc");
    deliveries[1] = datagram_send(broker, c, cb, cbgen, &cbdir, "def");
    deliveries[2] = datagram_send(broker, a, ab, abgen, &abdir, "ghi");
    struct unix_socket_diagnostic diagnostic;
    assert(unix_broker_diagnose(broker, b, bid, &diagnostic) == 0);
    assert(diagnostic.datagram_count == 3 && diagnostic.datagram_bytes == 192);
    assert(diagnostic.delivery == deliveries[0] && diagnostic.incoming.generation == abgen);
    assert(unix_broker_close(broker, a, aid) == 0);
    /* The unused A->D region goes away; queued A->B packets survive A. */
    assert(fixture->directions == 2);
    for (unsigned i = 0; i < 3; i++) {
        struct unix_delivery delivery;
        struct unix_direction direction;
        assert(unix_broker_dgram_head(broker, b, bid, &delivery, &direction) == 0);
        assert(delivery.id == deliveries[i] && delivery.length == 3);
        assert(delivery.credentials.pid == (i == 1 ? 403 : 401));
        /* Sender-owned descriptor/header corruption cannot poison the
         * destination's queue or change the authenticated source metadata. */
        struct unix_record *record = (void *)(direction.tx->data +
            (delivery.position & (UNIX_TRANSPORT_BYTES - 1u)));
        record->length = UINT32_MAX;
        uint64_t pending = UINT64_MAX;
        assert(unix_broker_pending(broker, b, bid, &pending) == 0 && pending == 3);
        assert(unix_broker_pending(broker, c, bid, &pending) == -EBADF);
        struct unix_read read;
        assert(unix_transport_packet_begin(direction.tx, direction.rx, delivery.generation,
            12, delivery.position, delivery.length, 0, delivery.operation, 3, &read) == 0);
        assert(memcmp(read.spans[0].base, &"abcdefghi"[i * 3], 3) == 0);
        assert(unix_broker_dgram_consume(broker, b, bid, delivery.id, &read, 1) == 0);
        assert(unix_transport_packet_begin(direction.tx, direction.rx, delivery.generation,
            12, delivery.position, delivery.length, 0, delivery.operation, 3, &read) == 0);
        assert(unix_broker_dgram_consume(broker, b, bid, delivery.id, &read, 0) == 0);
        assert(unix_broker_dgram_consume(broker, b, bid, delivery.id, &read, 0) == 0);
    }
    assert(fixture->directions == 1);
    struct unix_delivery delivery;
    struct unix_direction direction;
    assert(unix_broker_dgram_head(broker, b, bid, &delivery, &direction) == -EAGAIN);
    assert(unix_broker_diagnose(broker, b, bid, &diagnostic) == 0);
    assert(diagnostic.datagram_count == 0 && diagnostic.datagram_bytes == 0);
    assert(diagnostic.delivery == 0 && diagnostic.incoming.generation == 0);
    unix_broker_session_destroy(broker, a);
    unix_broker_session_destroy(broker, b);
    unix_broker_session_destroy(broker, c);
    unix_broker_session_destroy(broker, d);
    assert(fixture->directions == 0 && unix_broker_socket_count(broker) == 0);
}

static void datagram_disconnect(struct unix_broker *broker)
{
    struct unix_session *s = session(broker, 501);
    uint64_t a = datagram_socket(broker, s, 'a'), b = datagram_socket(broker, s, 'b');
    struct unix_address address = { .kind = UNIX_ADDRESS_ABSTRACT, .length = 1, .bytes = {'b'} };
    uint64_t route, generation;
    struct unix_direction direction, incoming;
    assert(unix_broker_dgram_route(broker, s, a, &address, &route, &generation, &direction) == 0);
    uint64_t dropped = datagram_send(broker, s, route, generation, &direction, "old");
    address.bytes[0] = 'a';
    assert(unix_broker_connect(broker, s, b, &address) == 0); /* first connect retains */
    struct unix_delivery delivery;
    struct unix_read stale, current;
    assert(unix_broker_dgram_head(broker, s, b, &delivery, &incoming) == 0 && delivery.id == dropped);
    assert(unix_transport_packet_begin(incoming.tx, incoming.rx, generation, 12,
        delivery.position, delivery.length, 0, delivery.operation, 3, &stale) == 0);
    /* Purge while a real reader owns RX. Corrupt sender metadata must not
     * determine how far to reclaim, and purge must not steal the lock. */
    direction.tx->published = UINT64_MAX;
    struct unix_address disconnect = {0};
    assert(unix_broker_connect(broker, s, b, &disconnect) == 0);
    assert(direction.rx->consumed == 64 && direction.rx->owner == 12);
    assert(unix_broker_dgram_consume(broker, s, b, dropped, &stale, 0) == -ESTALE);
    unix_transport_read_cancel(direction.rx, &stale);
    direction.tx->published = 64;
    uint64_t fresh = datagram_send(broker, s, route, generation, &direction, "new");
    assert(unix_broker_dgram_head(broker, s, b, &delivery, &incoming) == 0 && delivery.id == fresh);
    assert(unix_transport_packet_begin(incoming.tx, incoming.rx, generation, 12,
        delivery.position, delivery.length, 0, delivery.operation, 3, &current) == 0);
    assert(unix_broker_dgram_consume(broker, s, b, fresh, &current, 0) == 0);
    assert(unix_broker_dgram_consume(broker, s, b, dropped, &stale, 0) == -ESTALE);
    unix_broker_session_destroy(broker, s);
    assert(!unix_broker_socket_count(broker));
}

int main(void)
{
    struct fixture fixture = { .fail_after = -1 };
    const struct unix_broker_platform platform = { .context = &fixture,
        .direction_create = direction_create, .direction_destroy = direction_destroy,
        .notify = notify };
    struct unix_broker *broker = unix_broker_create(&platform);
    assert(broker);
    direct_and_lifetime(broker, &fixture);
    names_backlog_credentials(broker, &fixture);
    allocation_rollback(broker, &fixture);
    datagram_isolation_and_order(broker, &fixture);
    datagram_disconnect(broker);
    assert(fixture.notifications != 0);
    unix_broker_destroy(broker);
    puts("unix broker: connections, ownership, names, credentials, rollback, datagram isolation/order, diagnostics passed");
    return 0;
}
