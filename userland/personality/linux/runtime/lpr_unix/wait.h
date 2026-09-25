#pragma once
#include "client.h"
#include "../lpr_wait.h"
#include <unixd/notify.h>

_Static_assert(LPR_WAIT_GRAPH_MAX_LEAVES <= UNIX_WAIT_REGISTRATION_LIMIT,
    "local waiter cache must fit the service budget");

/* One waiting thread owns this object. The client is borrowed. Neither the
 * receive capability nor its ID/attempt may be inherited or serialized. */
struct lpr_unix_waiter {
    const struct lpr_unix_client *client;
    uint64_t process_token, owner, request;
    uint32_t id, attempt;
    int fd;
    unsigned next;
    /* Bound retained control-plane registrations by the existing poll graph
     * capacity. Active references are never evicted; idle entries own no FD
     * pin, socket reference, or shared-memory armed slot. */
    struct { uint64_t socket, identity; unsigned references; }
        watches[LPR_WAIT_GRAPH_MAX_LEAVES];
};

void lpr_unix_waiter_init(struct lpr_unix_waiter *waiter,
    const struct lpr_unix_client *client, uint64_t owner);
/* Closing the receive cap causes unixd to remove all watches and slots. */
void lpr_unix_waiter_destroy(struct lpr_unix_waiter *waiter);
/* identity belongs to the pinned local socket state, initially zero. It is
 * allocated once, not reused on close/reimport, and is not broker authority. */
int lpr_unix_waiter_watch(struct lpr_unix_waiter *waiter, uint64_t socket, uint64_t *identity);
/* Release an active reference after disarming shared slots. Keep only the
 * bounded broker registration; eviction or receive-close removes it. */
int lpr_unix_waiter_unwatch(struct lpr_unix_waiter *waiter, uint64_t socket);
/* Drain only this waiter's queue; bounded work returns EAGAIN under flooding.
 * Caller must recheck state even when no notification was present. */
int lpr_unix_waiter_drain(struct lpr_unix_waiter *waiter);
/* No automatic drain: the next arm/recheck cycle owns that operation. */
int lpr_unix_waiter_add_graph(struct lpr_unix_waiter *waiter, lpr_wait_graph_t *graph);

/* One blocking-I/O wait attempt on a watched direction. ready returns >0
 * for ready, 0 for not ready, <0 for an error. Always disarms before return,
 * including signal restart and timeout; 0 means retry I/O, not readiness.
 * bank must be the caller's writable bank, never the peer's RO mapping.
 * The same absolute deadline is retained across caller retries. */
int64_t lpr_unix_waiter_wait(struct lpr_unix_waiter *waiter,
    struct unix_wait_bank *bank, const uint64_t *first_changes,
    const uint64_t *second_changes, int (*ready)(void *), void *context,
    const lpr_wait_deadline_t *deadline);
