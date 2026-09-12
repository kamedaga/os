/* SPDX-License-Identifier: MIT */
#ifndef PACHA_GPUD_LAUNCH_H
#define PACHA_GPUD_LAUNCH_H

#include <kobox2/controller.h>
#include "launch_native.h"
#include "../kobox2_adapter/ipc.h"

struct gpud_launch_resources {
    uint64_t generation, resource_set_id, sandbox_id;
    uint8_t manifest_digest[KB2_DIGEST_SIZE];
    const struct gpud_launch_request *request;
};

struct gpud_launch_transaction {
    kb2_controller_t *controller;
    struct gpud_native_launch *launch;
    struct ph_ipc *ipc;
    uint64_t generation, action_token, resource_set_id, sandbox_id, native_generation;
    int launch_error, cleanup_error;
    unsigned int completed;
};

/* gpud only; never link this binding into the GPL sandbox. Single owner,
 * serialized with controller/IPC/launch mutations. Resources come from an
 * owner table; IDs are opaque, not FD casts. The owner must validate native
 * executable identity and pair its startup config with the selected closure.
 * Initially grant only the bootstrap channel; transfer device resources only
 * AFTER successful action completion. This keeps failed-launch rollback from
 * bypassing device revoke/reset. Source channel/config/image remain immutable
 * and borrowed during begin only; the three mutable objects stay borrowed.
 *
 * OK means a transaction was recorded, not LAUNCH complete. Native prepare or
 * start errors are stored in launch_error and must be driven through step.
 * Rejected binding arguments perform no launch and leave transaction empty. */
kb2_status_t gpud_launch_begin(struct gpud_launch_transaction *transaction,
    kb2_controller_t *controller, struct gpud_native_launch *launch,
    struct ph_ipc *ipc, const struct gpud_launch_resources *resources);
/* Match generation/token/IDs before touching native state. Successful START
 * advances only to TRANSFER, never READY/RUNNING. Failed launch revokes local
 * IPC admission, aborts, and reports failure only after all launch inventory
 * is gone. Cleanup EAGAIN returns ACTION_PENDING; other cleanup errors return
 * HOST_FAILURE with completed==0 and the action/ownership retained for retry.
 * A stale action retains inventory: its owner must resolve it, not overwrite
 * this object or a newly published generation. */
kb2_status_t gpud_launch_step(struct gpud_launch_transaction *transaction);

#endif
