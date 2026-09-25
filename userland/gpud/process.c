/* SPDX-License-Identifier: MIT */
#include "process.h"
#include <errno.h>

static kb2_status_t check_watch(const struct gpud_process_watch *watch, uint64_t generation) {
    if (!watch || !watch->controller || !watch->process || !watch->ipc || !generation)
        return KB2_STATUS_INVALID_ARGUMENT;
    if (watch->generation != generation || watch->process->generation != generation ||
        watch->ipc->generation != generation ||
        kb2_controller_generation(watch->controller) != generation)
        return KB2_STATUS_STALE_GENERATION;
    return KB2_STATUS_OK;
}

static int action_matches(const struct gpud_process_watch *watch, const kb2_action_t *action) {
    /* TERMINATE/REAP deliberately carry only a sandbox ID; resource actions
     * belong to the separate resource owner. TRANSFER carries both IDs. */
    const kb2_action_type_t type = kb2_action_type(action);
    uint64_t resource_id = type == KB2_ACTION_TERMINATE_SANDBOX ||
        type == KB2_ACTION_REAP_SANDBOX ? 0 : watch->resource_set_id;
    return action && kb2_action_generation(action) == watch->generation &&
        kb2_action_resource_set_id(action) == resource_id &&
        kb2_action_sandbox_id(action) == watch->sandbox_id;
}

static kb2_status_t native_result(struct gpud_process_watch *watch, int result) {
    watch->native_error = result;
    if (!result) return KB2_STATUS_OK;
    return result == -EAGAIN ? KB2_STATUS_ACTION_PENDING : KB2_STATUS_HOST_FAILURE;
}

kb2_status_t gpud_process_watch_init(struct gpud_process_watch *watch,
    kb2_controller_t *controller, struct gpud_native_process *process,
    struct ph_ipc *ipc, uint64_t resource_set_id, uint64_t sandbox_id) {
    if (!watch || watch->controller || !controller || !process || !ipc ||
        !resource_set_id || !sandbox_id || !process->fd || !ipc->admitted)
        return KB2_STATUS_INVALID_ARGUMENT;
    struct gpud_process_watch candidate = {.controller = controller, .process = process,
        .ipc = ipc, .generation = process->generation,
        .resource_set_id = resource_set_id, .sandbox_id = sandbox_id};
    kb2_status_t result = check_watch(&candidate, process->generation);
    if (result != KB2_STATUS_OK) return result;
    const kb2_action_t *action = kb2_controller_pending_action(controller);
    if (!action_matches(&candidate, action) ||
        kb2_action_type(action) != KB2_ACTION_TRANSFER_RESOURCES)
        return KB2_STATUS_INVALID_STATE;
    *watch = candidate;
    return KB2_STATUS_OK;
}

kb2_status_t gpud_process_observe(struct gpud_process_watch *watch, uint64_t generation) {
    kb2_status_t result = check_watch(watch, generation);
    if (result != KB2_STATUS_OK) return result;
    kb2_controller_t *controller = watch->controller;
    const kb2_action_t *action = kb2_controller_pending_action(controller);
    if (action && (!action_matches(watch, action) ||
        kb2_action_type(action) != KB2_ACTION_TRANSFER_RESOURCES))
        return KB2_STATUS_ACTION_PENDING;
    kb2_state_t state = kb2_controller_state(controller);
    if (!action && state != KB2_STATE_HANDSHAKING && state != KB2_STATE_RUNNING)
        return KB2_STATUS_INVALID_STATE;
    result = native_result(watch, gpud_native_process_poll(watch->process, generation));
    if (result != KB2_STATUS_OK) return result;
    result = native_result(watch, ph_ipc_revoke(watch->ipc, generation));
    if (result != KB2_STATUS_OK) return result;
    if (action) {
        return kb2_controller_complete_action(controller, generation, kb2_action_token(action),
            KB2_STATUS_HOST_FAILURE, 0, 0);
    }
    return kb2_controller_report_fault(controller, generation,
        KB2_FAULT_PROCESS_EXIT, watch->process->exit.code);
}

kb2_status_t gpud_process_action(struct gpud_process_watch *watch,
    uint64_t generation, uint64_t token, uint32_t kill_code) {
    kb2_status_t result = check_watch(watch, generation);
    if (result != KB2_STATUS_OK) return result;
    const kb2_action_t *action = kb2_controller_pending_action(watch->controller);
    if (!action || kb2_action_token(action) != token)
        return KB2_STATUS_STALE_ACTION;
    const kb2_action_type_t type = kb2_action_type(action);
    if (type != KB2_ACTION_TERMINATE_SANDBOX && type != KB2_ACTION_REAP_SANDBOX)
        return KB2_STATUS_INVALID_STATE;
    if (!action_matches(watch, action)) return KB2_STATUS_STALE_ACTION;
    result = native_result(watch, ph_ipc_revoke(watch->ipc, generation));
    if (result != KB2_STATUS_OK) return result;
    int status = type == KB2_ACTION_TERMINATE_SANDBOX ?
        gpud_native_process_terminate(watch->process, generation, kill_code) :
        gpud_native_process_release(watch->process, generation);
    result = native_result(watch, status);
    if (result != KB2_STATUS_OK) return result;
    return kb2_controller_complete_action(watch->controller, generation, token,
        KB2_STATUS_OK, 0, 0);
}
