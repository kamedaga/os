#pragma once

#include "unixd/ipc_protocol.h"

struct unix_broker;
struct unix_socket;
struct unix_session;

/* The broker owns the RW mappings and retains the original capabilities.
 * Attaching a client attenuates these rights; the peer never gets RW to
 * both halves. The platform layer is also the resource-admission boundary. */
struct unix_direction {
    struct unix_tx *tx;
    struct unix_rx *rx;
    int tx_fd;
    int rx_fd;
};

struct unix_broker_platform {
    void *context;
    int (*direction_create)(void *, uint64_t, uint32_t, struct unix_direction *);
    void (*direction_destroy)(void *, struct unix_direction *);
    int (*connection_create)(void *, uint64_t, uint32_t, struct unix_direction [2]);
    void (*connection_destroy)(void *, struct unix_direction [2]);
    void (*notify)(void *, uint64_t socket);
    void (*socket_destroy)(void *, uint64_t socket);
    /* External provider references are validated/adopted by the service,
     * never accepted as raw application-supplied native FD numbers. */
    int (*external_retain)(void *, uint64_t reference);
    void (*external_release)(void *, uint64_t reference);
};

struct unix_broker *unix_broker_create(const struct unix_broker_platform *platform);
void unix_broker_destroy(struct unix_broker *broker);
int unix_broker_session_create(struct unix_broker *broker,
    const struct unix_credentials *credentials, struct unix_session **out);
void unix_broker_session_destroy(struct unix_broker *broker, struct unix_session *session);
uint64_t unix_broker_session_id(const struct unix_session *session);
void unix_broker_session_identity(const struct unix_session *session, struct unix_credentials *out);
int unix_broker_session_credentials(struct unix_session *session,
    const struct unix_credentials *credentials);
int unix_broker_check_credentials(const struct unix_session *session,
    const struct unix_credentials *claimed);
int unix_broker_socket(struct unix_broker *broker, struct unix_session *session,
    uint32_t type, uint32_t flags, uint64_t *out);
int unix_broker_socketpair(struct unix_broker *broker, struct unix_session *session,
    uint32_t type, uint32_t flags, uint64_t out[2]);
int unix_broker_bind(struct unix_broker *broker, struct unix_session *session,
    uint64_t socket, const struct unix_address *address);
int unix_broker_path_check(struct unix_broker *broker, struct unix_session *session,
    uint64_t socket, int binding);
int unix_broker_listen(struct unix_broker *broker, struct unix_session *session,
    uint64_t socket, uint32_t backlog);
int unix_broker_connect(struct unix_broker *broker, struct unix_session *session,
    uint64_t socket, const struct unix_address *address);
int unix_broker_accept(struct unix_broker *broker, struct unix_session *session,
    uint64_t socket, uint64_t *out);
int unix_broker_name(struct unix_broker *broker, struct unix_session *session,
    uint64_t socket, int peer, struct unix_address *out);
int unix_broker_poll(struct unix_broker *broker, struct unix_session *session,
    uint64_t socket, uint32_t events, uint64_t *out);
int unix_broker_poll_sequence(struct unix_broker *, struct unix_session *, uint64_t,
    struct unix_poll_sequence *);
int unix_broker_pending(struct unix_broker *, struct unix_session *, uint64_t socket, uint64_t *out);
int unix_broker_attach(struct unix_broker *broker, struct unix_session *session,
    uint64_t socket, struct unix_attachment *out, struct unix_direction *tx,
    struct unix_direction *rx);
int unix_broker_retain(struct unix_broker *broker, struct unix_session *source,
    uint64_t socket, struct unix_session *destination);
int unix_broker_close(struct unix_broker *broker, struct unix_session *session,
    uint64_t socket);
int unix_broker_shutdown(struct unix_broker *broker, struct unix_session *session,
    uint64_t socket, unsigned how);
int unix_broker_set_flags(struct unix_broker *broker, struct unix_session *session,
    uint64_t socket, uint32_t flags);
int unix_broker_get_flags(struct unix_broker *, struct unix_session *, uint64_t, uint64_t *);
int unix_broker_error(struct unix_broker *, struct unix_session *, uint64_t, uint64_t *);
int unix_broker_options(struct unix_broker *, struct unix_session *, uint64_t,
    uint32_t mask, uint32_t values, uint64_t *);
int unix_broker_peercred(struct unix_broker *, struct unix_session *, uint64_t, struct unix_credentials *);
/* Thread identity must be assigned by the trusted registration path. */
void unix_broker_thread_died(struct unix_broker *broker, uint64_t owner);
size_t unix_broker_socket_count(const struct unix_broker *broker);
int unix_broker_diagnose(struct unix_broker *broker, struct unix_session *session,
    uint64_t socket, struct unix_socket_diagnostic *out);
/* Broker service helpers. Native FD numbers are never taken from shared memory. */
int unix_broker_can_notify(struct unix_broker *broker, struct unix_session *session,
    uint64_t source, uint64_t destination);
void unix_broker_wait_remove(struct unix_broker *broker, uint64_t socket,
    uint32_t notification);

struct unix_delivery {
    uint64_t id;
    uint64_t route;
    uint64_t generation;
    uint32_t length;
    uint32_t position;
    uint64_t operation;
    uint64_t ticket;
    struct unix_address source;
    struct unix_credentials credentials;
};

int unix_broker_dgram_route(struct unix_broker *broker, struct unix_session *session,
    uint64_t socket, const struct unix_address *address, uint64_t *route,
    uint64_t *generation, struct unix_direction *direction);
int unix_broker_dgram_commit(struct unix_broker *broker, struct unix_session *session,
    uint64_t route, struct unix_write *write, uint64_t *delivery);
int unix_broker_dgram_head(struct unix_broker *broker, struct unix_session *session,
    uint64_t socket, struct unix_delivery *delivery, struct unix_direction *direction);
int unix_broker_dgram_consume(struct unix_broker *broker, struct unix_session *session,
    uint64_t socket, uint64_t delivery, struct unix_read *read, int peek);
