/* SPDX-License-Identifier: MIT */
#include "lifecycle.h"
#include "lifecycle_message.h"
#include <errno.h>

enum { WORK_IDLE, WORK_READY, WORK_BUSY, WORK_DONE };

static int wait_ready_writable(struct ph_ipc *ipc) {
    struct pacha_pollfd event = {.fd = ipc->fd,
        .events = PACHA_FD_EVENT_WRITABLE | PACHA_FD_EVENT_HANGUP | PACHA_FD_EVENT_ERROR};
    for (;;) {
        event.revents = 0;
        /* The opening Linux task may migrate between native threads, so this
         * wait is transient rather than borrowing a thread's retained set. */
        long status = pacha_syscall4(PACHA_FD_SYSCALL_WAIT_MANY,
            (uintptr_t)&event, 1, PACHA_FD_WAIT_FOREVER, 0);
        if (status == PACHA_SYSCALL_ERR_NOT_READY && !event.revents) continue;
        if (status < 0) return (int)status;
        if (status != 1 || !event.revents || (event.revents & ~event.events)) return -EIO;
        // Closure wins even when a stale writable hint accompanies it.
        if (event.revents & PACHA_FD_EVENT_HANGUP) return -EPIPE;
        if (event.revents & PACHA_FD_EVENT_ERROR) return -EIO;
        return 0;
    }
}

static int ready(void *context) {
    struct ph_lifecycle *lifecycle = context;
    struct ph_ipc_packet packet = {.operation = PH_LIFECYCLE_READY,
        .generation = lifecycle->generation};
    for (;;) {
        int result = ph_ipc_send(lifecycle->ipc, &packet);
        if (!result) break;
        if (result != -EAGAIN) return result;
        /* Progress and READY share the bounded channel. A full diagnostic
         * queue is not a failed boot. Only the opening task owns IPC until
         * READY is enqueued; leave the receiver parked throughout the wait. */
        result = wait_ready_writable(lifecycle->ipc);
        if (result) return result;
    }
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

static int abort_monitor(void *context, int error) {
    struct ph_lifecycle *lifecycle = context;
    int expected = 0;

    if (!lifecycle->monitored || error >= 0)
        return -EINVAL;
    return atomic_compare_exchange_strong_explicit(&lifecycle->monitor_abort,
        &expected, error, memory_order_release, memory_order_relaxed) ?
        0 : -EALREADY;
}

static void return_to_receiver(struct ph_lifecycle *lifecycle, int result) {
    lifecycle->work_result = result;
    atomic_store_explicit(&lifecycle->work, WORK_DONE, memory_order_release);
    ph_wake(&lifecycle->work);
}

static int quiesce(struct ph_lifecycle *lifecycle, const struct ph_ipc_packet *packet) {
    if (!packet->correlation || packet->value || packet->fd_count) return -EPROTO;
    lifecycle->token = packet->correlation;
    return 1;
}

/* While BUSY, the receiver is parked and this task owns IPC and staging.
 * Check control before each ring request; a hot queue cannot hide QUIESCE.
 * An empty probe keeps ownership until the Linux loop ends its bounded poll.
 */
static int poll_work(void *context) {
    struct ph_lifecycle *lifecycle = context;
    if (atomic_load_explicit(&lifecycle->work, memory_order_acquire) != WORK_BUSY)
        return pending(context);
    if (lifecycle->service.poll_ready) {
        int queued = lifecycle->service.poll_ready(lifecycle->service.context);
        if (!queued) return 0;
        if (queued < 0) {
            return_to_receiver(lifecycle, queued);
            return 0;
        }
        /* Empty polls need no syscall. A queued request still checks native
         * control first, so continuous traffic cannot hide QUIESCE. */
    }
    struct ph_ipc_packet packet = {0};
    int result = ph_ipc_receive(lifecycle->ipc, lifecycle->generation, &packet);
    if (!result) {
        if (packet.operation == PH_LIFECYCLE_QUIESCE) {
            result = quiesce(lifecycle, &packet);
            PH_OK(ph_ipc_packet_release(&packet));
            return_to_receiver(lifecycle, result);
            return 0;
        }
        result = lifecycle->service.prepare(lifecycle->service.context, &packet);
        int closed = ph_ipc_packet_release(&packet);
        if (closed) {
            /* Match terminal receiver cleanup: never lose a capability
             * still owned by this local packet after a failed close. */
            PH_OK(ph_ipc_packet_release(&packet));
            if (result >= 0) result = closed;
        }
    } else if (result == -EAGAIN) {
        result = lifecycle->service.next(lifecycle->service.context);
    } else {
        return_to_receiver(lifecycle, result);
        return 0;
    }
    if (result == PH_LIFECYCLE_SERVICE_IDLE) return 0;
    if (result) {
        if (result > 0) result = -EPROTO;
        (void)lifecycle->service.release(lifecycle->service.context);
        return_to_receiver(lifecycle, result);
        return 0;
    }
    atomic_store_explicit(&lifecycle->work, WORK_READY, memory_order_release);
    return 2;
}

static void idle(void *context) {
    struct ph_lifecycle *lifecycle = context;
    if (atomic_load_explicit(&lifecycle->work, memory_order_acquire) == WORK_BUSY)
        return_to_receiver(lifecycle, 0);
}

static int dispatch(void *context, void *linux_service) {
    struct ph_lifecycle *lifecycle = context;
    unsigned int expected = WORK_READY;
    if (!atomic_compare_exchange_strong_explicit(&lifecycle->work, &expected, WORK_BUSY,
        memory_order_acquire, memory_order_relaxed)) return -EPROTO;
    int result = lifecycle->service.dispatch(lifecycle->service.context, linux_service);
    if (result > 0) result = -EPROTO;
    int completed = result ? result :
        lifecycle->service.complete(lifecycle->service.context, lifecycle->ipc);
    int closed = lifecycle->service.release(lifecycle->service.context);
    completed = completed ? completed : closed;
    /* A ring service retains ownership across the Linux loop's short poll.
     * No staging is reused until completion publication and release finish. */
    if (completed || !lifecycle->service.next)
        return_to_receiver(lifecycle, completed);
    /* Completion errors reach the core through terminal only after staging
     * has been released and the receiver has stopped admission. */
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
        atomic_store_explicit(&lifecycle->work, WORK_IDLE, memory_order_release);
        return result; /* The opening task has already released staging. */
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

static void async_signal_linux(struct ph_lifecycle *lifecycle) {
    unsigned state = atomic_load_explicit(&lifecycle->work, memory_order_relaxed);
    do {
        if (state == WORK_DONE) return;
    } while (!atomic_compare_exchange_weak_explicit(&lifecycle->work, &state,
        WORK_READY, memory_order_release, memory_order_relaxed));
    PH_OK(ph_task_ops.cpu_notify(0, KOBOX_LINUX_TASK_CONTROL_EVENT));
}

static int async_start(void *context, void *service) {
    struct ph_lifecycle *lifecycle = context;
    int result = lifecycle->service.start(lifecycle->service.context, service);
    return result > 0 ? -EPROTO : result;
}

static void async_fault(struct ph_lifecycle *lifecycle, int error) {
    int expected = 0;
    atomic_compare_exchange_strong_explicit(&lifecycle->async_error, &expected,
        error < 0 ? error : -EPROTO, memory_order_release, memory_order_relaxed);
    atomic_store_explicit(&lifecycle->quiescing, 1, memory_order_release);
}

static void async_unpark(struct ph_lifecycle *lifecycle, unsigned state) {
    atomic_store_explicit(&lifecycle->work, state, memory_order_release);
    /* Paired with the helper's arm/fenced recheck: neither side can miss
     * both the new state and the other's intent to sleep. */
    atomic_thread_fence(memory_order_seq_cst);
    if (atomic_load_explicit(&lifecycle->helper_parked, memory_order_acquire))
        ph_wake(&lifecycle->work);
}

/* Advances between control receives while no wake reports one. Every
 * advance in a request's round trip lies on the client's critical path, and
 * an empty native RECV there cost about as much as executing a stat. */
#define CONTROL_QUANTUM 16u

static int async_advance(void *context, void *service) {
    struct ph_lifecycle *lifecycle = context;
    atomic_exchange_explicit(&lifecycle->work, WORK_BUSY, memory_order_acquire);
    bool progressed = false;
    /* Check control once per bounded ring quantum. A hot data queue cannot
     * hide credential changes or QUIESCE from its sole owner. Between those,
     * control is received when the helper's wait ended: an idle owner sleeps
     * only after the helper waits on the control endpoint too. */
    bool control = atomic_exchange_explicit(&lifecycle->control_hint, 0,
        memory_order_acquire) || ++lifecycle->control_skips >= CONTROL_QUANTUM;
    if (control && !atomic_load_explicit(&lifecycle->quiescing, memory_order_acquire)) {
        lifecycle->control_skips = 0;
        struct ph_ipc_packet packet = {0};
        int result = ph_ipc_receive(lifecycle->ipc, lifecycle->generation, &packet);
        if (!result) {
            progressed = true;
            if (packet.operation == PH_LIFECYCLE_QUIESCE) {
                result = quiesce(lifecycle, &packet);
                if (result == 1) {
                    atomic_store_explicit(&lifecycle->quiescing, 1, memory_order_release);
                    result = 0;
                }
            } else {
                result = lifecycle->service.prepare(lifecycle->service.context, &packet);
                if (!result) {
                    result = lifecycle->service.complete(lifecycle->service.context,
                        lifecycle->ipc);
                    int released = lifecycle->service.release(lifecycle->service.context);
                    if (!result) result = released;
                } else if (result == PH_LIFECYCLE_SERVICE_IDLE) result = 0;
                else (void)lifecycle->service.release(lifecycle->service.context);
            }
            PH_OK(ph_ipc_packet_release(&packet));
        } else if (result == -EAGAIN) result = 0;
        if (result) async_fault(lifecycle, result);
    }
    bool stopping = atomic_load_explicit(&lifecycle->quiescing, memory_order_acquire) != 0;
    int result = lifecycle->service.advance(lifecycle->service.context, service,
        lifecycle->ipc, stopping);
    if (result == PH_LIFECYCLE_SERVICE_IDLE && stopping) {
        int error = atomic_load_explicit(&lifecycle->async_error, memory_order_acquire);
        atomic_store_explicit(&lifecycle->terminal, error ? error : 1, memory_order_release);
        async_unpark(lifecycle, WORK_DONE);
        PH_OK(lifecycle->service.wake(lifecycle->service.context));
        return 0;
    }
    if (result != 0 && result != PH_LIFECYCLE_SERVICE_PROGRESS) {
        async_fault(lifecycle, result);
        return PH_LIFECYCLE_SERVICE_PROGRESS;
    }
    if (!result && progressed) result = PH_LIFECYCLE_SERVICE_PROGRESS;
    if (!result) async_unpark(lifecycle, WORK_IDLE);
    return result;
}

static int async_finish(void *context, void *service) {
    struct ph_lifecycle *lifecycle = context;
    unsigned expected = 0;
    if (atomic_compare_exchange_strong_explicit(&lifecycle->start, &expected, 2,
        memory_order_release, memory_order_relaxed)) ph_wake(&lifecycle->start);
    /* READY failure leaves the helper unstarted. Normal shutdown interrupts
     * its non-consuming FD wait before any watched capability is closed. */
    async_unpark(lifecycle, WORK_DONE);
    PH_OK(lifecycle->service.wake(lifecycle->service.context));
    PH_OK(ph_thread_join(lifecycle->receiver));
    lifecycle->receiver = NULL;
    int result = lifecycle->service.finish(lifecycle->service.context, service);
    if (lifecycle->service.stop) {
        int stopped = lifecycle->service.stop(lifecycle->service.context);
        if (!result) result = stopped;
    }
    return result > 0 ? -EPROTO : result;
}

static long wait_receiver(struct ph_wait_retained *wait_set,
    struct pacha_pollfd *events, size_t count, uint64_t timeout) {
    for (;;) {
        long status = ph_wait_many_retained(wait_set, events, count, timeout);
        size_t ready = 0;
        for (size_t i = 0; i < count; ++i) ready += events[i].revents != 0;
        if (status > 0 && (size_t)status == ready) return status;
        if (status == PACHA_SYSCALL_ERR_NOT_READY && !ready) {
            if (timeout != PACHA_FD_WAIT_FOREVER) return status;
            continue;
        }
        return -EIO;
    }
}

static int receive_async(struct ph_lifecycle *lifecycle,
    struct ph_wait_retained *wait_set) {
    for (;;) {
        if (atomic_load_explicit(&lifecycle->work, memory_order_acquire) == WORK_DONE)
            return 0;
        atomic_store_explicit(&lifecycle->helper_parked, 1, memory_order_release);
        atomic_thread_fence(memory_order_seq_cst);
        unsigned state = atomic_load_explicit(&lifecycle->work, memory_order_acquire);
        if (state == WORK_DONE) return 0;
        if (state != WORK_IDLE ||
            atomic_load_explicit(&lifecycle->quiescing, memory_order_acquire)) {
            ph_wait(&lifecycle->work, state);
            atomic_store_explicit(&lifecycle->helper_parked, 0, memory_order_release);
            continue;
        }
        atomic_store_explicit(&lifecycle->helper_parked, 0, memory_order_release);
        int result;
        if (lifecycle->service.wait) {
            result = lifecycle->service.wait(lifecycle->service.context,
                lifecycle->ipc->fd, PACHA_FD_WAIT_FOREVER);
        } else {
            struct pacha_pollfd events[2] = {{.fd = lifecycle->service.wake_fd,
                .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP},
                {.fd = lifecycle->ipc->fd,
                .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP}};
            result = wait_receiver(wait_set, events, 2, PACHA_FD_WAIT_FOREVER) < 0 ? -EIO : 0;
        }
        if (atomic_load_explicit(&lifecycle->work, memory_order_acquire) == WORK_DONE)
            return 0;
        /* Which endpoint ended the wait is not reported; control is cheap to
         * check once per helper wake, which is never per request. */
        atomic_store_explicit(&lifecycle->control_hint, 1, memory_order_release);
        if (result) {
            async_fault(lifecycle, result);
            async_signal_linux(lifecycle);
        } else {
            unsigned expected = WORK_IDLE;
            if (atomic_compare_exchange_strong_explicit(&lifecycle->work, &expected,
                WORK_READY, memory_order_release, memory_order_relaxed))
                PH_OK(ph_task_ops.cpu_notify(0, KOBOX_LINUX_TASK_CONTROL_EVENT));
        }
    }
}

static void *receive(void *context) {
    struct ph_lifecycle *lifecycle = context;
    /* ph_thread_create gives this receiver a stable native thread. */
    struct ph_wait_retained wait_set = {0};
    unsigned int start;
    while (!(start = atomic_load_explicit(&lifecycle->start, memory_order_acquire)))
        ph_wait(&lifecycle->start, 0);
    if (start == 2) {
        /* Module init/READY failed before admission. */
        if (!lifecycle->service.advance && lifecycle->service.stop)
            PH_OK(lifecycle->service.stop(lifecycle->service.context));
        return NULL;
    }
    if (lifecycle->service.advance) {
        (void)receive_async(lifecycle, &wait_set);
        return NULL;
    }
    int result;
    struct ph_ipc_packet packet = {0};
    for (;;) {
        if (lifecycle->monitored &&
            (result = atomic_load_explicit(&lifecycle->monitor_abort,
                memory_order_acquire)))
            break;
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
        if (lifecycle->service.wait) {
            result = lifecycle->service.wait(lifecycle->service.context,
                lifecycle->ipc->fd, lifecycle->monitored ? 1000 : PACHA_FD_WAIT_FOREVER);
            if (result) break;
            continue;
        }
        struct pacha_pollfd events[2] = {{.fd = lifecycle->ipc->fd,
            .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP}};
        size_t count = 1;
        if (lifecycle->service.wake_fd >= 16)
            events[count++] = (struct pacha_pollfd){
                .fd = lifecycle->service.wake_fd,
                .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP};
        if (lifecycle->monitored) {
            /* The Linux opening task can abort for temperature or elapsed
             * time while this native receiver has no incoming IPC. Polling
             * only this inspection profile bounds its join to one second.
             */
            long waited = wait_receiver(&wait_set, events, count, 1000);
            size_t ready = 0;
            for (size_t i = 0; i < count; ++i)
                ready += events[i].revents != 0;
            if (waited == 0 ||
                (waited == PACHA_SYSCALL_ERR_NOT_READY && !ready))
                continue;
            if (waited < 0 || (size_t)waited != ready) {
                result = -EIO;
                break;
            }
        } else {
            PH_CHECK(wait_receiver(&wait_set, events, count, PACHA_FD_WAIT_FOREVER) > 0);
        }
    }
    if (!result) {
        if (packet.operation != PH_LIFECYCLE_QUIESCE)
            result = -EPROTO;
        else
            result = quiesce(lifecycle, &packet);
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

static int start_service(struct ph_lifecycle *lifecycle, struct ph_ipc *ipc,
    const struct ph_lifecycle_service *service, bool monitored) {
    if (!lifecycle || lifecycle->generation || !ipc || !ipc->generation || !ipc->admitted)
        return -EINVAL;
    if (service && (!service->context || !service->prepare ||
        !service->complete || !service->release)) return -EINVAL;
    if (service && service->advance) {
        if (!service->start || !service->finish || !service->wake ||
            service->wake_fd < 16 || service->dispatch || service->next ||
            service->poll_ready || monitored) return -EINVAL;
    } else if (service && (!service->dispatch || service->start || service->finish ||
        service->wake)) return -EINVAL;
    *lifecycle = (struct ph_lifecycle){.ipc = ipc, .generation = ipc->generation,
        .port = {.size = sizeof(lifecycle->port), .context = lifecycle, .ready = ready,
            .pending = pending, .dispatch = service ? dispatch : NULL,
            .abort = monitored ? abort_monitor : NULL},
        .monitored = monitored};
    if (service) lifecycle->service = *service;
    if (service && service->advance) {
        lifecycle->port.dispatch = NULL;
        lifecycle->port.start = async_start;
        lifecycle->port.advance = async_advance;
        lifecycle->port.finish = async_finish;
    }
    if (service && service->next) {
        lifecycle->port.poll = poll_work;
        lifecycle->port.idle = idle;
    }
    atomic_init(&lifecycle->start, 0);
    atomic_init(&lifecycle->terminal, 0);
    atomic_init(&lifecycle->monitor_abort, 0);
    atomic_init(&lifecycle->work, WORK_IDLE);
    atomic_init(&lifecycle->quiescing, 0);
    atomic_init(&lifecycle->async_error, 0);
    atomic_init(&lifecycle->helper_parked, 0);
    atomic_init(&lifecycle->control_hint, 1); /* First advance receives control. */
    lifecycle->control_skips = 0;
    if (!atomic_is_lock_free(&lifecycle->terminal) || !atomic_is_lock_free(&lifecycle->start) ||
        !atomic_is_lock_free(&lifecycle->work) ||
        !atomic_is_lock_free(&lifecycle->monitor_abort) ||
        !atomic_is_lock_free(&lifecycle->quiescing) ||
        !atomic_is_lock_free(&lifecycle->async_error) ||
        !atomic_is_lock_free(&lifecycle->helper_parked) ||
        !atomic_is_lock_free(&lifecycle->control_hint)) {
        *lifecycle = (struct ph_lifecycle){0};
        return -EOPNOTSUPP;
    }
    lifecycle->receiver = ph_thread_create(receive, lifecycle);
    return 0;
}

int ph_lifecycle_start_service(struct ph_lifecycle *lifecycle, struct ph_ipc *ipc,
    const struct ph_lifecycle_service *service) {
    return start_service(lifecycle, ipc, service, false);
}

int ph_lifecycle_start(struct ph_lifecycle *lifecycle, struct ph_ipc *ipc) {
    return ph_lifecycle_start_service(lifecycle, ipc, NULL);
}

int ph_lifecycle_start_inspected(struct ph_lifecycle *lifecycle,
    struct ph_ipc *ipc) {
    return start_service(lifecycle, ipc, NULL, true);
}

int ph_lifecycle_start_monitored_service(struct ph_lifecycle *lifecycle,
    struct ph_ipc *ipc, const struct ph_lifecycle_service *service) {
    return start_service(lifecycle, ipc, service, true);
}

void ph_lifecycle_finish(struct ph_lifecycle *lifecycle) {
    PH_CHECK(lifecycle);
    if (lifecycle->service.advance && !lifecycle->receiver) return;
    PH_CHECK(lifecycle->receiver);
    /* Only unstarted receiver cancellation is permitted. If READY succeeded,
     * modules_run cannot return until its control-event completion fired. */
    unsigned int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&lifecycle->start, &expected, 2,
        memory_order_release, memory_order_relaxed)) ph_wake(&lifecycle->start);
    PH_OK(ph_thread_join(lifecycle->receiver));
    lifecycle->receiver = NULL;
    /* Async start failed before READY: no admission/worker survives start's
     * rollback, but native mappings still belong to the original caller. */
    if (lifecycle->service.advance && lifecycle->service.stop)
        PH_OK(lifecycle->service.stop(lifecycle->service.context));
}
