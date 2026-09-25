#pragma once
#include "broker.h"

enum unix_ticket_state {
    UNIX_TICKET_PREPARING = 1,
    UNIX_TICKET_QUEUED = 2,
    UNIX_TICKET_DELIVERED = 3,
    UNIX_TICKET_CANCELLED = 4,
    UNIX_TICKET_DROPPED = 5, /* send committed, receiver discarded the queue */
};

/* Exactly one field is nonzero. External is a service-owned provider escrow
 * reference; its descriptor/capabilities are not part of the shared ring. */
struct unix_right_ref { uint64_t socket; uint64_t external; };
struct unix_rights_info {
    uint64_t ticket;
    uint64_t source;
    uint64_t destination;
    uint64_t generation;
    uint64_t operation;
    uint32_t position;
    uint32_t length;
    uint32_t count;
    uint32_t state;
    uint32_t credentials_present;
    struct unix_credentials credentials;
};

int unix_broker_rights_prepare(struct unix_broker *, struct unix_session *,
    uint64_t source, uint64_t owner, uint64_t operation, unsigned total,
    const struct unix_credentials *claimed, uint64_t *ticket);
int unix_broker_rights_prepare_route(struct unix_broker *, struct unix_session *,
    uint64_t source, uint64_t route, uint64_t owner, uint64_t operation, unsigned total,
    const struct unix_credentials *claimed, uint64_t *ticket);
/* Append is ordered and retry-safe for the same offset and references.
 * The caller retains its external references, including on failure. */
int unix_broker_rights_append(struct unix_broker *, struct unix_session *,
    uint64_t ticket, unsigned offset, const struct unix_right_ref *, unsigned count);
/* Validates the append range before native capability adoption. Existing
 * ranges return their original references; a new range sets present=0. */
int unix_broker_rights_appended(struct unix_broker *, struct unix_session *,
    uint64_t ticket, unsigned offset, struct unix_right_ref *, unsigned count, int *present);
int unix_broker_rights_commit(struct unix_broker *, struct unix_session *,
    uint64_t ticket, struct unix_write *write);
/* Caller must cancel its shared write first; cancellation cannot evict a
 * still-live ring owner. Death recovery is a separate trusted path. */
int unix_broker_rights_cancel(struct unix_broker *, struct unix_session *, uint64_t ticket);
/* Read the immutable escrow while holding the destination RX lock at the
 * associated record. Import stays hidden until consume has succeeded. */
int unix_broker_rights_head(struct unix_broker *, struct unix_session *, uint64_t socket,
    uint64_t ticket, const struct unix_read *, struct unix_rights_info *,
    struct unix_right_ref *refs, unsigned capacity);
int unix_broker_rights_socket(struct unix_broker *, struct unix_session *, uint64_t destination,
    uint64_t ticket, const struct unix_read *, unsigned index,
    struct unix_attachment *, struct unix_direction *, struct unix_direction *);
/* Transfers a prefix to receiver ownership and discards the remainder.
 * Zero is ordinary read/MSG_CTRUNC discard. Retries of the same receiver
 * operation return the saved outcome without consuming/importing again. */
int unix_broker_rights_consume(struct unix_broker *, struct unix_session *, uint64_t socket,
    uint64_t ticket, uint64_t owner, uint64_t operation, struct unix_read *, unsigned take, int peek);
/* Read-only recovery of a completed receive, including a dead thread's
 * receipt. Never allocates progress or starts a new operation. */
int unix_broker_rights_received(struct unix_broker *, struct unix_session *, uint64_t socket,
    uint64_t ticket, uint64_t owner, uint64_t operation, unsigned take, int peek);
/* One outstanding completed receive per thread. ACK permits the next one;
 * its monotonic operation watermark rejects old replays after receipts retire. */
int unix_broker_rights_ack(struct unix_broker *, struct unix_session *, uint64_t ticket,
    uint64_t owner, uint64_t operation, int receiving);
size_t unix_broker_ticket_count(const struct unix_broker *);
