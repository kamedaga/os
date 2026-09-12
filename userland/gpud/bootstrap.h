/* SPDX-License-Identifier: MIT */
#ifndef PACHA_GPUD_BOOTSTRAP_H
#define PACHA_GPUD_BOOTSTRAP_H

#include <kobox2/controller.h>
#include "../kobox2_adapter/bootstrap.h"

/* Resolved by gpud's resource owner, never by casting opaque controller IDs
 * to native handles. All entries are borrowed until this action terminates.
 * The launch owner must give the receiver independent counts/digests and
 * retain the package/resource backing through revoke/reset/reap/release. */
struct gpud_bootstrap_resources {
    uint64_t generation, resource_set_id, sandbox_id;
    uint8_t manifest_digest[KB2_DIGEST_SIZE];
    const struct ph_bootstrap_item *items;
    size_t artifact_count, resource_handle_count;
};

struct gpud_bootstrap_transfer {
    kb2_controller_t *controller;
    struct ph_ipc *ipc;
    struct ph_bootstrap_sender sender;
    uint64_t action_token, resource_set_id, sandbox_id;
    int native_error;
    unsigned int completed;
};

/* gpud only: this header and controller binding must not enter the GPL
 * sandbox build. Serialize with every controller mutation and IPC operation.
 * Zero-initialize transfer; success borrows controller, ipc and resources. */
kb2_status_t gpud_bootstrap_begin(struct gpud_bootstrap_transfer *transfer,
    kb2_controller_t *controller, struct ph_ipc *ipc,
    const struct gpud_bootstrap_resources *resources);
/* One message per call; ACTION_PENDING includes backpressure (native_error
 * distinguishes EAGAIN/ENOMEM). Only FINISH enqueue completes the action.
 * Success leaves the controller HANDSHAKING, never RUNNING.
 * Fatal send failures revoke IPC admission and fail the pending action. The
 * owner must then drive controller stop/cleanup, not release live resources. */
kb2_status_t gpud_bootstrap_step(struct gpud_bootstrap_transfer *transfer);

#endif
