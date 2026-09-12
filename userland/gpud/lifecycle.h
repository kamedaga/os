/* SPDX-License-Identifier: MIT */
#ifndef PACHA_GPUD_LIFECYCLE_H
#define PACHA_GPUD_LIFECYCLE_H

#include "process.h"

struct gpud_lifecycle {
    struct gpud_process_watch *watch;
    struct ph_ipc_packet incoming;
    uint64_t generation, quiesce_token;
    unsigned int ready, quiesce_sent, completed;
    int native_error;
};

/* gpud only. Single owner; borrow an initialized process watch after TRANSFER
 * completed. Serializes all controller/IPC/process mutations. This READY is
 * module-closure readiness; publishing a DRM service additionally requires
 * the selected service's device/protocol handshake. */
kb2_status_t gpud_lifecycle_init(struct gpud_lifecycle *lifecycle, struct gpud_process_watch *watch);
kb2_status_t gpud_lifecycle_ready(struct gpud_lifecycle *lifecycle);
/* Drive only the current QUIESCE action. Send the exact token once; then WAIT
 * must prove normal exit/code=0. EOF, enqueue success or peer claims are not
 * that proof. Backpressure/wait preserve the action and borrowed resources.
 * Failure closes admission and leaves controller cleanup to TERMINATE then
 * the resource owner. QUIESCE completion does not prove DMA revoke/reset. */
kb2_status_t gpud_lifecycle_quiesce(struct gpud_lifecycle *lifecycle, uint64_t token);
/* Release only owned rejected capabilities, including after failed READY
 * cleanup. May be retried; never closes borrowed IPC/process/controller. */
int gpud_lifecycle_release(struct gpud_lifecycle *lifecycle);

#endif
