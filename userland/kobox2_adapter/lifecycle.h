/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_LIFECYCLE_H
#define PACHA_KOBOX_LIFECYCLE_H

#include "host.h"
#include "ipc.h"
#include "boot/lifecycle.h"

enum { PH_LIFECYCLE_SERVICE_IDLE = 1, PH_LIFECYCLE_SERVICE_PROGRESS = 2 };

struct ph_lifecycle_service {
    void *context;
    /* Optional asynchronous execution. The opening Linux task owns control,
     * admission and resource reclamation. FS data workers may publish through
     * request-owned completion tickets before reclamation; other services
     * retain owner publication. Workers own private requests;
     * the native helper only waits for readiness, never consumes IPC/rings.
     * advance returns 0 idle, 2 progress, 1 drained only while quiescing, or
     * a negative fault. Faults stop admission but advance must drain leases
     * before modules can unload. finish joins workers before stop unmaps.
     * wake interrupts the helper's private wake_fd for shutdown; worker
     * completion wakes the Linux owner directly, without native IPC.
     */
    int (*start)(void *context, void *linux_service);
    int (*advance)(void *context, void *linux_service, struct ph_ipc *ipc,
        bool quiescing);
    int (*finish)(void *context, void *linux_service);
    int (*wake)(void *context);
    /* The receiver prepares the first request, then transfers exclusive
     * IPC/staging ownership to the opening task. For a ring service that
     * task also prepares subsequent requests until its bounded poll ends.
     * release retires staging after completion, or failed preparation. */
    /* May retain a received capability by replacing its fd with NO_FD;
     * lifecycle releases all entries that remain owned by the packet. */
    int (*prepare)(void *context, struct ph_ipc_packet *packet);
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
    /* Optional non-consuming ring peek, only while the opening task owns
     * staging. Empty burst polling can avoid a native receive syscall; the
     * bounded Linux poll still returns IPC ownership to the receiver. This
     * is never admission: control is checked before next consumes a chain. */
    int (*poll_ready)(void *context);
    /* After admission ends and no dispatch is active, including failures.
     * Async: opening Linux task, after helper/worker join. Otherwise receiver.
     * Start failure must roll back Linux resources before returning. */
    int (*stop)(void *context);
    /* Optional preallocated native doorbell. next drains and validates it;
     * lifecycle only includes it in the receiver's blocking wait. */
    int wake_fd;
    /* Synchronous: exclusive staging owner. Async: readiness helper only;
     * snapshot endpoint numbers under a short metadata lock, never access
     * mutable ring/request state or retain client pointers across the wait.
     * Dynamic ring services can wait on their
     * registered endpoints plus control without a fixed client-count ceiling.
     * Return zero after a wake/timeout, negative on transport failure; next
     * drains hints and rechecks authoritative rings before any admission. */
    int (*wait)(void *context, int control_fd, uint64_t timeout_ticks);
};

struct ph_lifecycle {
    struct kobox_linux_lifecycle port;
    struct ph_ipc *ipc;
    struct ph_task *receiver;
    atomic_uint start;
    atomic_int terminal;
    atomic_int monitor_abort;
    atomic_uint work;
    atomic_uint quiescing;
    atomic_int async_error;
    atomic_uint helper_parked;
    int work_result;
    struct ph_lifecycle_service service;
    uint64_t generation, token;
    bool monitored;
};

/* After bootstrap/package verification, on a registered task with core TLS
 * and CPU machinery ready. Owns a native receiver thread; borrows the sole
 * management endpoint. No other caller may receive/send on it until finish.
 * pending() is an atomic load only, safe for the Linux control-event IRQ.
 * ready() sends once, then releases the receiver; neither callback allocates
 * or enters Linux. A transport error is terminal, not graceful quiescence. */
int ph_lifecycle_start(struct ph_lifecycle *lifecycle, struct ph_ipc *ipc);
/* AMDGPU inspection only: a Linux-side sensor/time abort also retires the
 * native receiver, so normal module cleanup cannot hang at join. */
int ph_lifecycle_start_inspected(struct ph_lifecycle *lifecycle,
    struct ph_ipc *ipc);
int ph_lifecycle_start_monitored_service(struct ph_lifecycle *lifecycle,
    struct ph_ipc *ipc, const struct ph_lifecycle_service *service);
int ph_lifecycle_start_service(struct ph_lifecycle *lifecycle, struct ph_ipc *ipc,
    const struct ph_lifecycle_service *service);
/* After modules_run returns (including init failure). Joins the native
 * receiver before caller closes IPC/package. Does not detach DMA or IRQs. */
void ph_lifecycle_finish(struct ph_lifecycle *lifecycle);

#endif
