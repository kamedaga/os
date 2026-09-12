/* SPDX-License-Identifier: MIT */
#include "lifecycle.h"
#include "lifecycle_message.h"
#include <errno.h>

enum { WORK_IDLE, WORK_READY, WORK_BUSY, WORK_DONE };

static int ready(void *context) {
    struct ph_lifecycle *lifecycle = context;
    struct ph_ipc_packet packet = {.operation = PH_LIFECYCLE_READY,
        .generation = lifecycle->generation};
    int result = ph_ipc_send(lifecycle->ipc, &packet);
    if (result) return result;
    atomic_store_explicit(&lifecycle->start, 1, memory_order_release);
    ph_wake(&lifecycle->start);
    return 0;
}

static int pending(void *context) {
    struct ph_lifecycle *lifecycle = context;
    int terminal = atomic_load_explicit(&lifecycle->terminal, memory_order_acquire);
    if (terminal) return terminal;
    return atomic_load_explicit(&lifecycle->work, memory_order_acquire) == WORK_READY ? 2 : 0;
}

static int dispatch(void *context, void *linux_service) {
    struct ph_lifecycle *lifecycle = context;
    unsigned int expected = WORK_READY;
    if (!atomic_compare_exchange_strong_explicit(&lifecycle->work, &expected, WORK_BUSY,
        memory_order_acquire, memory_order_relaxed)) return -EPROTO;
    int result = lifecycle->service.dispatch(lifecycle->service.context, linux_service);
    if (result > 0) result = -EPROTO;
    lifecycle->work_result = result;
    atomic_store_explicit(&lifecycle->work, WORK_DONE, memory_order_release);
    ph_wake(&lifecycle->work);
    return result;
}

static int run_prepared(struct ph_lifecycle *lifecycle, int result) {
    if (result == PH_LIFECYCLE_SERVICE_IDLE && lifecycle->service.next) return 0;
    if (result > 0) result = -EPROTO;
    if (!result) {
        atomic_store_explicit(&lifecycle->work, WORK_READY, memory_order_release);
        PH_OK(ph_task_ops.cpu_notify(0, KOBOX_LINUX_TASK_CONTROL_EVENT));
        unsigned int state;
        while ((state = atomic_load_explicit(&lifecycle->work, memory_order_acquire)) != WORK_DONE)
            ph_wait(&lifecycle->work, state);
        result = lifecycle->work_result;
        if (!result) result = lifecycle->service.complete(lifecycle->service.context, lifecycle->ipc);
        atomic_store_explicit(&lifecycle->work, WORK_IDLE, memory_order_release);
    }
    int closed = lifecycle->service.release(lifecycle->service.context);
    return result ? result : closed;
}

static int run_service(struct ph_lifecycle *lifecycle, struct ph_ipc_packet *packet) {
    int result = lifecycle->service.prepare(lifecycle->service.context, packet);
    int closed = ph_ipc_packet_release(packet);
    if (closed && result >= 0) result = closed;
    return run_prepared(lifecycle, result);
}

static void *receive(void *context) {
    struct ph_lifecycle *lifecycle = context;
    unsigned int start;
    while (!(start = atomic_load_explicit(&lifecycle->start, memory_order_acquire)))
        ph_wait(&lifecycle->start, 0);
    if (start == 2) {
        /* Module init/READY failed before admission. */
        if (lifecycle->service.stop) PH_OK(lifecycle->service.stop(lifecycle->service.context));
        return NULL;
    }
    int result;
    struct ph_ipc_packet packet = {0};
    for (;;) {
        result = ph_ipc_receive(lifecycle->ipc, lifecycle->generation, &packet);
        if (!result) {
            if (packet.operation == PH_LIFECYCLE_QUIESCE) break;
            if (!lifecycle->service.prepare) { result = -EPROTO; break; }
            result = run_service(lifecycle, &packet);
            if (result) break;
            continue;
        }
        if (result != -EAGAIN) break;
        /* Check control between requests so continuous data publication
         * cannot starve QUIESCE. next arms/rechecks before the blocking wait. */
        if (lifecycle->service.next) {
            result = lifecycle->service.next(lifecycle->service.context);
            if (result != PH_LIFECYCLE_SERVICE_IDLE) {
                result = run_prepared(lifecycle, result);
                if (result) break;
                continue;
            }
        }
        struct pacha_pollfd event = {.fd = lifecycle->ipc->fd,
            .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP};
        (void)ph_wait_readable(&event, 1);
    }
    if (!result) {
        if (packet.operation != PH_LIFECYCLE_QUIESCE || !packet.correlation || packet.value || packet.fd_count)
            result = -EPROTO;
        else {
            lifecycle->token = packet.correlation;
            result = 1;
        }
    }
    /* Even rejected ancillary FDs retain ownership until their closes
     * succeed. Cleanup failure is fatal to this sandbox, never a leaked FD. */
    PH_OK(ph_ipc_packet_release(&packet));
    if (lifecycle->service.stop) {
        int stopped = lifecycle->service.stop(lifecycle->service.context);
        if (stopped) result = stopped < 0 ? stopped : -EPROTO;
    }
    atomic_store_explicit(&lifecycle->terminal, result, memory_order_release);
    PH_OK(ph_task_ops.cpu_notify(0, KOBOX_LINUX_TASK_CONTROL_EVENT));
    return NULL;
}

int ph_lifecycle_start_service(struct ph_lifecycle *lifecycle, struct ph_ipc *ipc,
    const struct ph_lifecycle_service *service) {
    if (!lifecycle || lifecycle->generation || !ipc || !ipc->generation || !ipc->admitted)
        return -EINVAL;
    if (service && (!service->context || !service->prepare || !service->dispatch ||
        !service->complete || !service->release)) return -EINVAL;
    *lifecycle = (struct ph_lifecycle){.ipc = ipc, .generation = ipc->generation,
        .port = {.size = sizeof(lifecycle->port), .context = lifecycle, .ready = ready,
            .pending = pending, .dispatch = service ? dispatch : NULL}};
    if (service) lifecycle->service = *service;
    atomic_init(&lifecycle->start, 0);
    atomic_init(&lifecycle->terminal, 0);
    atomic_init(&lifecycle->work, WORK_IDLE);
    if (!atomic_is_lock_free(&lifecycle->terminal) || !atomic_is_lock_free(&lifecycle->start) ||
        !atomic_is_lock_free(&lifecycle->work)) {
        *lifecycle = (struct ph_lifecycle){0};
        return -EOPNOTSUPP;
    }
    lifecycle->receiver = ph_thread_create(receive, lifecycle);
    return 0;
}

int ph_lifecycle_start(struct ph_lifecycle *lifecycle, struct ph_ipc *ipc) {
    return ph_lifecycle_start_service(lifecycle, ipc, NULL);
}

void ph_lifecycle_finish(struct ph_lifecycle *lifecycle) {
    PH_CHECK(lifecycle && lifecycle->receiver);
    /* Only unstarted receiver cancellation is permitted. If READY succeeded,
     * modules_run cannot return until its control-event completion fired. */
    unsigned int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&lifecycle->start, &expected, 2,
        memory_order_release, memory_order_relaxed)) ph_wake(&lifecycle->start);
    PH_OK(ph_thread_join(lifecycle->receiver));
    lifecycle->receiver = NULL;
}
