/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_LIFECYCLE_H
#define PACHA_KOBOX_LIFECYCLE_H

#include "host.h"
#include "ipc.h"
#include "boot/lifecycle.h"

enum { PH_LIFECYCLE_SERVICE_IDLE = 1 };

struct ph_lifecycle_service {
    void *context;
    /* The receiver prepares the first request, then transfers exclusive
     * IPC/staging ownership to the opening task. For a ring service that
     * task also prepares subsequent requests until its bounded poll ends.
     * release retires staging after completion, or failed preparation. */
    int (*prepare)(void *context, const struct ph_ipc_packet *packet);
    int (*release)(void *context);
    /* Opening Linux task, in order: dispatch executes the request, then
     * complete publishes its response without another thread handoff.
     * The receiver does not touch staging or IPC during these callbacks;
     * release follows on the same task before the next request is admitted. */
    int (*dispatch)(void *context, void *linux_service);
    int (*complete)(void *context, struct ph_ipc *ipc);
    /* Optional ring service: next snapshots another available request, or
     * arms notifications and returns IDLE after an empty recheck. prepare
     * may also return IDLE for an establishment/duplicate wake packet.
     * No work is inferred from notification counts. Both run only on the
     * current exclusive staging owner, never from an IRQ callback.
     */
    int (*next)(void *context);
    /* After admission ends and no dispatch is active, including failures.
     * Retires persistent channel mappings; called once by the receiver. */
    int (*stop)(void *context);
    /* Optional preallocated native doorbell. next drains and validates it;
     * lifecycle only includes it in the receiver's blocking wait. */
    int wake_fd;
};

struct ph_lifecycle {
    struct kobox_linux_lifecycle port;
    struct ph_ipc *ipc;
    struct ph_task *receiver;
    atomic_uint start;
    atomic_int terminal;
    atomic_uint work;
    int work_result;
    struct ph_lifecycle_service service;
    uint64_t generation, token;
};

/* After bootstrap/package verification, on a registered task with core TLS
 * and CPU machinery ready. Owns a native receiver thread; borrows the sole
 * management endpoint. No other caller may receive/send on it until finish.
 * pending() is an atomic load only, safe for the Linux control-event IRQ.
 * ready() sends once, then releases the receiver; neither callback allocates
 * or enters Linux. A transport error is terminal, not graceful quiescence. */
int ph_lifecycle_start(struct ph_lifecycle *lifecycle, struct ph_ipc *ipc);
int ph_lifecycle_start_service(struct ph_lifecycle *lifecycle, struct ph_ipc *ipc,
    const struct ph_lifecycle_service *service);
/* After modules_run returns (including init failure). Joins the native
 * receiver before caller closes IPC/package. Does not detach DMA or IRQs. */
void ph_lifecycle_finish(struct ph_lifecycle *lifecycle);

#endif
