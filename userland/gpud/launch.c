/* SPDX-License-Identifier: MIT */
#include "launch.h"
#include <kobox2/closure.h>
#include <pacha/syscall.h>
#include <errno.h>
#include <string.h>

static int owns_inventory(const struct gpud_native_launch *launch) {
    return launch->process.fd || launch->thread_fd || launch->scratch_fd || launch->scratch_view;
}

kb2_status_t gpud_launch_begin(struct gpud_launch_transaction *transaction,
    kb2_controller_t *controller, struct gpud_native_launch *launch,
    struct ph_ipc *ipc, const struct gpud_launch_resources *resources) {
    if (!transaction || transaction->controller || !controller || !launch || !ipc ||
        !resources || !resources->request || !resources->resource_set_id || !resources->sandbox_id)
        return KB2_STATUS_INVALID_ARGUMENT;
    const kb2_action_t *action = kb2_controller_pending_action(controller);
    if (!action || kb2_action_type(action) != KB2_ACTION_LAUNCH_SANDBOX || owns_inventory(launch) ||
        launch->prepared || launch->started || launch->failed) return KB2_STATUS_INVALID_STATE;
    uint64_t generation = kb2_action_generation(action);
    if (resources->generation != generation || resources->request->generation != generation ||
        ipc->generation != generation || launch->process.generation >= generation)
        return KB2_STATUS_STALE_GENERATION;
    if (!ipc->admitted || !ipc->fd || kb2_action_resource_set_id(action) != resources->resource_set_id)
        return KB2_STATUS_RESOURCE_DENIED;
    if (!kb2_closure_uses_native_lifecycle(kb2_action_closure(action)))
        return KB2_STATUS_INVALID_CONFIGURATION;
    uint8_t expected[KB2_DIGEST_SIZE];
    kb2_status_t result = kb2_action_copy_digest(action, KB2_DIGEST_MANIFEST, expected, sizeof(expected));
    if (result != KB2_STATUS_OK) return result;
    if (memcmp(expected, resources->manifest_digest, sizeof(expected))) return KB2_STATUS_RESOURCE_DENIED;
    const struct gpud_launch_request *request = resources->request;
    if (request->grant_count != 1 || !request->grants) return KB2_STATUS_RESOURCE_DENIED;
    const struct pacha_process_fd_grant *grant = request->grants;
    const uint64_t allowed = PH_IPC_CHANNEL_RIGHTS | PACHA_FD_RIGHT_CLOSE;
    if (grant->source_fd < 16 || grant->source_fd >= PACHA_FD_TABLE_LIMIT ||
        grant->source_fd == (uint64_t)ipc->fd || grant->target_fd < 16 ||
        grant->target_fd >= PACHA_FD_TABLE_LIMIT || grant->rights & ~allowed ||
        (grant->rights & PH_IPC_CHANNEL_RIGHTS) != PH_IPC_CHANNEL_RIGHTS ||
        grant->flags & ~PACHA_FD_FLAG_CLOEXEC) return KB2_STATUS_RESOURCE_DENIED;
    struct pacha_fd_info info = {0};
    long native = pacha_syscall2(PACHA_FD_SYSCALL_GET_INFO, grant->source_fd, (uintptr_t)&info);
    if (native) return KB2_STATUS_HOST_FAILURE;
    if (info.kind != PACHA_FD_KIND_CHANNEL ||
        (info.rights & (grant->rights | PACHA_FD_RIGHT_TRANSFER)) !=
            (grant->rights | PACHA_FD_RIGHT_TRANSFER)) return KB2_STATUS_RESOURCE_DENIED;
    *transaction = (struct gpud_launch_transaction){.controller = controller,
        .launch = launch, .ipc = ipc, .generation = generation,
        .action_token = kb2_action_token(action), .resource_set_id = resources->resource_set_id,
        .sandbox_id = resources->sandbox_id};
    int error = gpud_native_launch_prepare(launch, request);
    if (!error) error = gpud_native_launch_start(launch, generation);
    transaction->native_generation = launch->process.generation;
    transaction->launch_error = error;
    return KB2_STATUS_OK;
}

kb2_status_t gpud_launch_step(struct gpud_launch_transaction *transaction) {
    if (!transaction || !transaction->controller) return KB2_STATUS_INVALID_ARGUMENT;
    kb2_controller_t *controller = transaction->controller;
    struct gpud_native_launch *launch = transaction->launch;
    uint64_t generation = transaction->generation;
    if (kb2_controller_generation(controller) != generation || transaction->ipc->generation != generation ||
        launch->process.generation != transaction->native_generation) return KB2_STATUS_STALE_GENERATION;
    if (transaction->completed) return KB2_STATUS_INVALID_STATE;
    const kb2_action_t *action = kb2_controller_pending_action(controller);
    if (!action || kb2_action_type(action) != KB2_ACTION_LAUNCH_SANDBOX ||
        kb2_action_token(action) != transaction->action_token ||
        kb2_action_resource_set_id(action) != transaction->resource_set_id || kb2_action_sandbox_id(action))
        return KB2_STATUS_STALE_ACTION;
    if (!transaction->launch_error) {
        if (!launch->started || !launch->process.fd) return KB2_STATUS_INVALID_STATE;
        int observed = gpud_native_process_poll(&launch->process, generation);
        /* Immediate exit never loses an unreported native process. Roll it
         * back before the controller publishes any sandbox ownership. */
        if (observed != -EAGAIN) transaction->launch_error = observed ? observed : -ECHILD;
    }
    if (transaction->launch_error) {
        (void)ph_ipc_revoke(transaction->ipc, generation);
        int cleanup = launch->process.generation == generation ?
            gpud_native_launch_abort(launch, generation) : 0;
        transaction->cleanup_error = cleanup;
        if (cleanup) return cleanup == -EAGAIN ? KB2_STATUS_ACTION_PENDING : KB2_STATUS_HOST_FAILURE;
        if (owns_inventory(launch)) return KB2_STATUS_HOST_FAILURE;
    }
    kb2_status_t outcome = transaction->launch_error ? KB2_STATUS_HOST_FAILURE : KB2_STATUS_OK;
    kb2_status_t result = kb2_controller_complete_action(controller, generation,
        transaction->action_token, outcome, 0, outcome == KB2_STATUS_OK ? transaction->sandbox_id : 0);
    transaction->completed = 1;
    return result;
}
