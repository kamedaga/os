/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_BOOTSTRAP_H
#define PACHA_KOBOX_BOOTSTRAP_H

#include "ipc.h"

#define PH_BOOTSTRAP_MAX_ARTIFACTS 64u
#define PH_BOOTSTRAP_MAX_RESOURCES 64u
#define PH_BOOTSTRAP_MAX_ITEMS (2u + PH_BOOTSTRAP_MAX_ARTIFACTS + PH_BOOTSTRAP_MAX_RESOURCES)
#define PH_BOOTSTRAP_BLOB_RIGHTS (PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_MAP_READ)

/* Native launch protocol, not GPU wire opcodes. The two first blobs contain
 * the existing canonical closure manifest and resource grant. Artifacts then
 * follow manifest order, resources follow transfer_handle_index order.
 * Each message carries one FD and its logical byte length (VMOs are padded).
 * Counts and expected digests must come from independent launch-owner state. */
enum ph_bootstrap_operation {
    PH_BOOTSTRAP_BLOB = 1,
    PH_BOOTSTRAP_RESOURCE,
    PH_BOOTSTRAP_FINISH,
};

/* Independent launch-owner state, not identity learned from peer messages. */
struct ph_package_identity {
    uint64_t generation;
    uint8_t manifest_digest[32], grant_digest[32];
};

struct ph_bootstrap_item {
    struct pacha_ipc_fd capability;
    uint64_t size;
};

struct ph_bootstrap_sender {
    uint64_t generation;
    const struct ph_bootstrap_item *items;
    size_t artifact_count, resource_count, next;
    unsigned int finished;
};

struct ph_bootstrap_receiver {
    uint64_t generation;
    size_t artifact_count, resource_count, received;
    unsigned int complete, failed;
    struct ph_bootstrap_item items[PH_BOOTSTRAP_MAX_ITEMS];
    struct ph_ipc_packet pending;
};

/* All objects are single-owner and initially zero. Sender borrows input FDs
 * and items until finished/abandoned; it always SHAREs, never partially MOVEs
 * an ownership bundle. finished means enqueued, NOT sandbox READY. Advance is
 * nonblocking and moves next only on successful enqueue. */
int ph_bootstrap_sender_init(struct ph_bootstrap_sender *sender, uint64_t generation,
    const struct ph_bootstrap_item *items, size_t artifact_count, size_t resource_count);
int ph_bootstrap_send_next(struct ph_bootstrap_sender *sender, struct ph_ipc *ipc);

int ph_bootstrap_receiver_init(struct ph_bootstrap_receiver *receiver, uint64_t generation,
    size_t artifact_count, size_t resource_count);
/* Consume at most one native message. EAGAIN/ENOMEM preserve partial state for
 * retry. Other receive/validation errors revoke the borrowed IPC endpoint and
 * reclaim all staged FDs; failed closes retain ownership for abort retry.
 * No item is usable before complete. Even then it is only a received bundle:
 * the caller MUST validate immutable package digests, grant generation and
 * per-resource native kind/rights before loading or importing any item.
 * Read-only transfer rights alone do not freeze an external VMO writer. */
int ph_bootstrap_receive_next(struct ph_bootstrap_receiver *receiver, struct ph_ipc *ipc);
/* Abort closes admission and owned staging; it does not destroy borrowed ipc.
 * Release is for a completed bundle after its consumers have stopped, or for
 * retrying failed cleanup. Success zeros the receiver. Accepted item ownership
 * may be taken explicitly by replacing its FD with PH_IPC_NO_FD. */
int ph_bootstrap_abort(struct ph_bootstrap_receiver *receiver, struct ph_ipc *ipc);
int ph_bootstrap_release(struct ph_bootstrap_receiver *receiver);

#endif
