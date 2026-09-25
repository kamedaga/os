#pragma once
#include "unixd/transfer.h"
#include "pacha/ipc.h"

struct unix_escrow_entry;
struct unix_escrow {
    struct unix_escrow_entry *entries;
    uint64_t next_id;
};

void unix_escrow_init(struct unix_escrow *);
void unix_escrow_destroy(struct unix_escrow *);
/* On success takes ownership of the specified incoming native FDs and zeros
 * their slots so the RPC cleanup will not close the retained escrow. */
int unix_escrow_capture(struct unix_escrow *, const struct unix_transfer_item *,
    struct pacha_ipc_fd *incoming, unsigned count, uint64_t *reference);
int unix_escrow_retain(struct unix_escrow *, uint64_t reference);
void unix_escrow_release(struct unix_escrow *, uint64_t reference);
int unix_escrow_export(struct unix_escrow *, uint64_t reference,
    struct unix_transfer_item *, struct pacha_ipc_fd *, unsigned capacity);
int unix_escrow_matches(struct unix_escrow *, uint64_t reference,
    const struct unix_transfer_item *);
