/* SPDX-License-Identifier: MIT */
#include "lifecycle.h"
#include "../kobox2_adapter/lifecycle_message.h"
#include <errno.h>

static kb2_status_t check(const struct gpud_lifecycle *lifecycle) {
    if (!lifecycle || !lifecycle->watch) return KB2_STATUS_INVALID_ARGUMENT;
    const struct gpud_process_watch *watch = lifecycle->watch;
    if (lifecycle->generation != watch->generation ||
        watch->generation != watch->process->generation || watch->generation != watch->ipc->generation ||
        watch->generation != kb2_controller_generation(watch->controller)) return KB2_STATUS_STALE_GENERATION;
    return KB2_STATUS_OK;
}

static kb2_status_t ready_failure(struct gpud_lifecycle *lifecycle, int error) {
    lifecycle->native_error = error;
    struct gpud_process_watch *watch = lifecycle->watch;
    (void)ph_ipc_revoke(watch->ipc, lifecycle->generation);
    /* Retain rejected capabilities in the object when close must be retried. */
    int cleanup = ph_ipc_packet_release(&lifecycle->incoming);
    if (cleanup) {
        lifecycle->native_error = cleanup;
        return KB2_STATUS_HOST_FAILURE;
    }
    (void)kb2_controller_report_fault(watch->controller, lifecycle->generation, KB2_FAULT_PROTOCOL,
        (uint64_t)(unsigned int)-error);
    return KB2_STATUS_HOST_FAILURE;
}

kb2_status_t gpud_lifecycle_init(struct gpud_lifecycle *lifecycle, struct gpud_process_watch *watch) {
    if (!lifecycle || lifecycle->watch || !watch || !watch->controller || !watch->process || !watch->ipc)
        return KB2_STATUS_INVALID_ARGUMENT;
    struct gpud_lifecycle candidate = {.watch = watch, .generation = watch->generation};
    kb2_status_t result = check(&candidate);
    if (result != KB2_STATUS_OK) return result;
    if (kb2_controller_state(watch->controller) != KB2_STATE_HANDSHAKING ||
        kb2_controller_pending_action(watch->controller) || !watch->ipc->admitted || !watch->process->fd)
        return KB2_STATUS_INVALID_STATE;
    *lifecycle = candidate;
    return KB2_STATUS_OK;
}

kb2_status_t gpud_lifecycle_ready(struct gpud_lifecycle *lifecycle) {
    kb2_status_t status = check(lifecycle);
    if (status != KB2_STATUS_OK) return status;
    struct gpud_process_watch *watch = lifecycle->watch;
    if (lifecycle->ready || kb2_controller_pending_action(watch->controller) ||
        kb2_controller_state(watch->controller) != KB2_STATE_HANDSHAKING) return KB2_STATUS_INVALID_STATE;
    if (lifecycle->native_error && !watch->ipc->admitted)
        return ready_failure(lifecycle, lifecycle->native_error);
    int result = ph_ipc_receive(watch->ipc, lifecycle->generation, &lifecycle->incoming);
    lifecycle->native_error = result;
    if (result == -EAGAIN || result == -ENOMEM) return KB2_STATUS_ACTION_PENDING;
    if (result) return ready_failure(lifecycle, result);
    const struct ph_ipc_packet *packet = &lifecycle->incoming;
    if (packet->operation != PH_LIFECYCLE_READY || packet->correlation || packet->value || packet->fd_count)
        return ready_failure(lifecycle, -EPROTO);
    result = gpud_native_process_poll(watch->process, lifecycle->generation);
    if (!result) {
        (void)ph_ipc_revoke(watch->ipc, lifecycle->generation);
        (void)kb2_controller_report_fault(watch->controller, lifecycle->generation,
            KB2_FAULT_PROCESS_EXIT, watch->process->exit.code);
        return KB2_STATUS_HOST_FAILURE;
    }
    if (result != -EAGAIN) return ready_failure(lifecycle, result);
    status = kb2_controller_report_ready(watch->controller, lifecycle->generation);
    if (status == KB2_STATUS_OK) lifecycle->ready = 1;
    return status;
}

int gpud_lifecycle_release(struct gpud_lifecycle *lifecycle) {
    if (!lifecycle) return -EINVAL;
    return ph_ipc_packet_release(&lifecycle->incoming);
}

kb2_status_t gpud_lifecycle_quiesce(struct gpud_lifecycle *lifecycle, uint64_t token) {
    kb2_status_t status = check(lifecycle);
    if (status != KB2_STATUS_OK) return status;
    struct gpud_process_watch *watch = lifecycle->watch;
    if (lifecycle->completed) return KB2_STATUS_INVALID_STATE;
    const kb2_action_t *action = kb2_controller_pending_action(watch->controller);
    if (!action || kb2_action_token(action) != token ||
        (lifecycle->quiesce_token && lifecycle->quiesce_token != token)) return KB2_STATUS_STALE_ACTION;
    if (kb2_action_type(action) != KB2_ACTION_QUIESCE_SANDBOX ||
        kb2_action_sandbox_id(action) != watch->sandbox_id ||
        kb2_action_resource_set_id(action) != watch->resource_set_id)
        return KB2_STATUS_INVALID_STATE;
    lifecycle->quiesce_token = token;
    int result = 0;
    if (!lifecycle->quiesce_sent) {
        struct ph_ipc_packet request = {.operation = PH_LIFECYCLE_QUIESCE,
            .generation = lifecycle->generation, .correlation = token};
        result = ph_ipc_send(watch->ipc, &request);
        if (!result) lifecycle->quiesce_sent = 1;
    }
    if (!result) {
        result = gpud_native_process_poll(watch->process, lifecycle->generation);
        if (!result && (watch->process->exit.state != GPUD_PROCESS_EXITED || watch->process->exit.code))
            result = -ECHILD;
    }
    lifecycle->native_error = result;
    if (result == -EAGAIN || result == -ENOMEM) return KB2_STATUS_ACTION_PENDING;
    (void)ph_ipc_revoke(watch->ipc, lifecycle->generation);
    status = kb2_controller_complete_action(watch->controller, lifecycle->generation, token,
        result ? KB2_STATUS_HOST_FAILURE : KB2_STATUS_OK, 0, 0);
    lifecycle->completed = 1;
    return status;
}
