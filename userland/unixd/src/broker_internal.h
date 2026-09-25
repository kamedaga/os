#pragma once
#include "broker.h"

struct unix_session {
    struct unix_session *next;
    uint64_t id;
    struct unix_credentials credentials;
};
struct unix_owner {
    struct unix_owner *next;
    struct unix_session *session;
    uint32_t references;
};
struct unix_connection {
    unsigned references;
    uint64_t generation;
    struct unix_direction directions[2];
};
struct unix_route {
    struct unix_route *next;
    struct unix_socket *source;
    struct unix_socket *destination;
    uint64_t id;
    unsigned queued;
    struct unix_direction direction;
};
struct unix_datagram {
    struct unix_datagram *next;
    struct unix_route *route;
    struct unix_delivery delivery;
};
struct unix_socket {
    struct unix_socket *next;
    struct unix_socket *peer;
    struct unix_socket *pending_next;
    struct unix_socket *pending_head;
    struct unix_socket *pending_tail;
    struct unix_owner *owners;
    struct unix_connection *connection;
    uint64_t id;
    uint32_t type;
    uint32_t flags;
    uint32_t backlog;
    uint32_t pending_count;
    uint32_t datagram_bytes;
    uint32_t shutdown; /* DGRAM: bit 0 receive, bit 1 send; queued data survives. */
    uint32_t error;
    uint32_t options;
    uint64_t dgram_peer;
    uint64_t connect_wait_listener;
    uint64_t consumed_delivery;
    struct unix_poll_sequence poll_sequence;
    uint64_t space_sequence;
    struct unix_datagram *datagram_head;
    struct unix_datagram *datagram_tail;
    unsigned listening;
    unsigned bound;
    unsigned side;
    unsigned reachable;
    struct unix_credentials credentials;
    struct unix_credentials peer_credentials;
    struct unix_address address;
    struct unix_address peer_address;
};
struct unix_ticket;
struct unix_right_progress;
struct unix_broker {
    struct unix_broker_platform platform;
    struct unix_session *sessions;
    struct unix_socket *sockets;
    struct unix_route *routes;
    struct unix_ticket *tickets;
    struct unix_right_progress *rights_progress;
    uint64_t next_id;
    uint64_t next_poll_event;
    size_t socket_count;
    size_t ticket_count;
};

struct unix_socket *unix_broker_lookup(struct unix_broker *, uint64_t);
struct unix_route *unix_broker_route_lookup(struct unix_broker *, uint64_t);
struct unix_socket *unix_broker_owned(struct unix_broker *, struct unix_session *, uint64_t);
int unix_broker_import_socket(struct unix_broker *, struct unix_session *, uint64_t);
int unix_broker_socket_attachment(struct unix_socket *, struct unix_attachment *,
    struct unix_direction *, struct unix_direction *);
int unix_broker_dgram_enqueue(struct unix_broker *, struct unix_route *, struct unix_write *,
    uint64_t ticket, uint64_t operation, const struct unix_credentials *, uint64_t *delivery);
int unix_broker_dgram_finish(struct unix_broker *, struct unix_socket *, struct unix_read *, int peek);
void unix_broker_collect(struct unix_broker *);
void unix_rights_mark_roots(struct unix_broker *);
int unix_rights_mark_edges(struct unix_broker *);
void unix_rights_sweep(struct unix_broker *);
void unix_rights_drop_datagram(struct unix_broker *, uint64_t ticket);
void unix_rights_session_died(struct unix_broker *, uint64_t session);
void unix_rights_thread_died(struct unix_broker *, uint64_t owner);
void unix_rights_destroy(struct unix_broker *);
