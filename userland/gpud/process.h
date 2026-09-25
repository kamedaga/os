/* SPDX-License-Identifier: MIT */
#ifndef PACHA_GPUD_PROCESS_H
#define PACHA_GPUD_PROCESS_H

#include <kobox2/controller.h>
#include "process_native.h"
#include "../kobox2_adapter/ipc.h"

/* gpud controller side only. IDs are owner-assigned, never native FD casts.
 * All pointers are borrowed until REAP; serialize with controller and IPC.
 * The owner attaches after publishing a successful LAUNCH action and before
 * driving TRANSFER. This does not create/start a process or claim READY. */
struct gpud_process_watch {
    kb2_controller_t *controller;
    struct gpud_native_process *process;
    struct ph_ipc *ipc;
    uint64_t generation, resource_set_id, sandbox_id;
    int native_error;
};

kb2_status_t gpud_process_watch_init(struct gpud_process_watch *watch,
    kb2_controller_t *controller, struct gpud_native_process *process,
    struct ph_ipc *ipc, uint64_t resource_set_id, uint64_t sandbox_id);
/* Observe native exit, close IPC admission, then report the controller fault.
 * An in-flight TRANSFER is failed first; the owner must subsequently stop or
 * restart the controller. Other pending actions are left to their driver.
 * Native EAGAIN leaves state unchanged and returns ACTION_PENDING. */
kb2_status_t gpud_process_observe(struct gpud_process_watch *watch, uint64_t generation);
/* Drive only TERMINATE and REAP, matching the exact action token and IDs.
 * Resource REVOKE/RESET must be completed by the resource owner, not here.
 * QUIESCE requires a sandbox protocol handshake and is not synthesized by KILL.
 * A native failure leaves the action pending and inventory owned for retry. */
kb2_status_t gpud_process_action(struct gpud_process_watch *watch,
    uint64_t generation, uint64_t token, uint32_t kill_code);

#endif
