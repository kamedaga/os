#pragma once
#include "client.h"

/* Local mapping cache, not the socket ownership reference. Fork may inherit
 * these data VMOs, but must register its own socket owner before user code.
 * Never serialize these virtual addresses in an exec/SCM transfer record. */
struct lpr_unix_mapping {
    uint64_t socket;
    uint64_t generation;
    uint64_t peer;
    uint32_t type;
    uint32_t flags;
    struct unix_credentials peer_credentials;
    struct unix_tx *outgoing_tx;
    const struct unix_rx *outgoing_rx;
    const struct unix_tx *incoming_tx;
    struct unix_rx *incoming_rx;
    int fds[2];
};

/* Fresh/closed output only. ATTACH does not create another broker owner.
 * STREAM/SEQPACKET map own RW and peer RO; DGRAM uses route attachments. */
int lpr_unix_mapping_attach(const struct lpr_unix_client *client, uint64_t socket,
    uint32_t expected_type, uint64_t request_id, struct lpr_unix_mapping *out);
void lpr_unix_mapping_destroy(struct lpr_unix_mapping *mapping);
/* Adopts all direction caps, including on error. Used by authenticated CLAIM. */
int lpr_unix_mapping_import(const struct unix_attachment *attachment,
    const struct pacha_ipc_fd *caps, unsigned count, struct lpr_unix_mapping *out);

/* A DGRAM route has separate TX/RX VMOs; the receiver may only write RX.
 * Mapping caps are PRIVATE, without DUP/TRANSFER. Cached mappings are not
 * socket owners and are never serialized into exec/SCM transfer records. */
struct lpr_unix_route_mapping {
    struct unix_tx *tx;
    struct unix_rx *rx;
    int fds[2];
};
int lpr_unix_route_import(const struct pacha_ipc_fd *caps, unsigned count,
    uint64_t generation, int writing, struct lpr_unix_route_mapping *out);
void lpr_unix_route_destroy(struct lpr_unix_route_mapping *mapping);
