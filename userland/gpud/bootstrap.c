/* SPDX-License-Identifier: MIT */
#include "bootstrap.h"
#include <kobox2/closure.h>
#include <errno.h>
#include <string.h>

kb2_status_t gpud_bootstrap_begin(struct gpud_bootstrap_transfer *transfer,
    kb2_controller_t *controller, struct ph_ipc *ipc,
    const struct gpud_bootstrap_resources *resources) {
    if (!transfer || transfer->controller || !controller || !ipc || !resources)
        return KB2_STATUS_INVALID_ARGUMENT;
    const kb2_action_t *action = kb2_controller_pending_action(controller);
    if (!action || kb2_action_type(action) != KB2_ACTION_TRANSFER_RESOURCES)
        return KB2_STATUS_INVALID_STATE;
    const uint64_t generation = kb2_action_generation(action);
    if (resources->generation != generation || ipc->generation != generation)
        return KB2_STATUS_STALE_GENERATION;
    if (!ipc->admitted || resources->resource_set_id != kb2_action_resource_set_id(action) ||
        resources->sandbox_id != kb2_action_sandbox_id(action)) return KB2_STATUS_RESOURCE_DENIED;
    const kb2_closure_t *closure = kb2_action_closure(action);
    if (!kb2_closure_uses_native_lifecycle(closure) ||
        resources->artifact_count != kb2_closure_artifact_count(closure))
        return KB2_STATUS_INVALID_CONFIGURATION;
    uint8_t expected[KB2_DIGEST_SIZE];
    kb2_status_t status = kb2_action_copy_digest(action, KB2_DIGEST_MANIFEST,
        expected, sizeof(expected));
    if (status != KB2_STATUS_OK) return status;
    if (memcmp(expected, resources->manifest_digest, sizeof(expected)))
        return KB2_STATUS_RESOURCE_DENIED;
    struct ph_bootstrap_sender sender = {0};
    int result = ph_bootstrap_sender_init(&sender, generation, resources->items,
        resources->artifact_count, resources->resource_handle_count);
    if (result) return KB2_STATUS_INVALID_CONFIGURATION;
    *transfer = (struct gpud_bootstrap_transfer){.controller = controller, .ipc = ipc,
        .sender = sender, .action_token = kb2_action_token(action),
        .resource_set_id = resources->resource_set_id, .sandbox_id = resources->sandbox_id};
    return KB2_STATUS_OK;
}

kb2_status_t gpud_bootstrap_step(struct gpud_bootstrap_transfer *transfer) {
    if (!transfer || !transfer->controller) return KB2_STATUS_INVALID_ARGUMENT;
    kb2_controller_t *controller = transfer->controller;
    const uint64_t generation = transfer->sender.generation;
    if (kb2_controller_generation(controller) != generation ||
        transfer->ipc->generation != generation) return KB2_STATUS_STALE_GENERATION;
    if (transfer->completed) return KB2_STATUS_INVALID_STATE;
    const kb2_action_t *action = kb2_controller_pending_action(controller);
    if (!action || kb2_action_type(action) != KB2_ACTION_TRANSFER_RESOURCES ||
        kb2_action_token(action) != transfer->action_token ||
        kb2_action_resource_set_id(action) != transfer->resource_set_id ||
        kb2_action_sandbox_id(action) != transfer->sandbox_id) return KB2_STATUS_STALE_ACTION;
    int result = ph_bootstrap_send_next(&transfer->sender, transfer->ipc);
    transfer->native_error = result;
    if (result == -EAGAIN || result == -ENOMEM) return KB2_STATUS_ACTION_PENDING;
    if (result) {
        /* Own generation was checked above; never revoke a recycled endpoint
         * in response to an old action callback. Resource FDs remain borrowed. */
        (void)ph_ipc_revoke(transfer->ipc, generation);
        kb2_status_t failure = result == -EACCES || result == -EPERM ?
            KB2_STATUS_RESOURCE_DENIED : KB2_STATUS_HOST_FAILURE;
        kb2_status_t status = kb2_controller_complete_action(controller, generation,
            transfer->action_token, failure, 0, 0);
        transfer->completed = 1;
        return status;
    }
    if (!transfer->sender.finished) return KB2_STATUS_ACTION_PENDING;
    kb2_status_t status = kb2_controller_complete_action(controller, generation,
        transfer->action_token, KB2_STATUS_OK, 0, 0);
    transfer->completed = 1;
    return status;
}
