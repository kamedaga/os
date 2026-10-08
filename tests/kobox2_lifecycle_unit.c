/* SPDX-License-Identifier: MIT */
#include "../userland/kobox2_adapter/lifecycle.h"
#include "../userland/kobox2_adapter/lifecycle_message.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static pthread_t receiver;
static struct ph_task task;
static struct ph_lifecycle lifecycle;
static struct ph_ipc_packet packets[4];
static size_t next_packet;
static unsigned int prepared, dispatched, completed, released, notifications;
static int failure;
static int ring_mode;
static unsigned int ring_pending, ring_packets, ring_stops, ring_waits;
static int peek_result, peek_quiesce;
static unsigned peek_next_calls;
static atomic_uint async_jobs, async_started_jobs, async_done, async_returned, async_freed;
static atomic_uint async_consumed, async_wake_calls;
static atomic_uint async_stop_requested, async_unblock, async_wakes, async_started, async_finished;
static int async_inject;
static int ready_failure, startup_failure;
static unsigned startup_started, startup_finished, startup_stopped;

static void async_changed(void) {
    pthread_mutex_lock(&lock);
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
}

long pacha_syscall4(uint64_t op, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    /* Inspection-only timed waits are outside this normal-lifecycle fixture;
     * retaining a hard failure makes accidentally entering that path visible. */
    (void)op;(void)a;(void)b;(void)c;(void)d;abort();
}

_Noreturn void ph_fail(const char *file, unsigned int line, uint64_t result) {
    fprintf(stderr, "%s:%u: %llu\n", file, line, (unsigned long long)result);
    abort();
}

void ph_wait(atomic_uint *word, unsigned int expected) {
    pthread_mutex_lock(&lock);
    while (atomic_load_explicit(word, memory_order_acquire) == expected)
        pthread_cond_wait(&changed, &lock);
    pthread_mutex_unlock(&lock);
}

void ph_wake(atomic_uint *word) {
    (void)word;
    pthread_mutex_lock(&lock);
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
}

struct ph_task *ph_thread_create(void *(*entry)(void *), void *argument) {
    assert(!pthread_create(&receiver, NULL, entry, argument));
    return &task;
}

int ph_thread_join(void *handle) {
    assert(handle == &task);
    return pthread_join(receiver, NULL);
}

static int notify(uint32_t cpu, enum kobox_linux_task_notification kind) {
    assert(cpu == 0 && kind == KOBOX_LINUX_TASK_CONTROL_EVENT);
    pthread_mutex_lock(&lock);
    ++notifications;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    return 0;
}

const struct kobox_linux_task_host_operations ph_task_ops = {.cpu_notify = notify};

int ph_ipc_send(struct ph_ipc *ipc, struct ph_ipc_packet *packet) {
    assert(ipc->generation == packet->generation && packet->operation == PH_LIFECYCLE_READY);
    return ready_failure;
}

int ph_ipc_receive(struct ph_ipc *ipc, uint64_t generation, struct ph_ipc_packet *packet) {
    if (ring_mode == 10) {
        assert(!pthread_equal(pthread_self(), receiver));
        assert(ipc->generation == generation && next_packet < 3);
        if (next_packet < 2) {
            *packet = (struct ph_ipc_packet){.operation = 0x200,
                .generation = generation, .correlation = ++next_packet};
            atomic_store(&async_consumed, (unsigned)next_packet);
            return 0;
        }
        if (!atomic_load(&async_stop_requested)) return -EAGAIN;
        if (async_inject == 2) return -EPIPE;
        if (async_inject && async_inject != 4) return -EAGAIN;
        *packet = (struct ph_ipc_packet){.operation = PH_LIFECYCLE_QUIESCE,
            .generation = generation, .correlation = async_inject == 4 ? 0 : 99};
        ++next_packet;
        atomic_store(&async_consumed, (unsigned)next_packet);
        return 0;
    }
    if (ring_mode == 8) {
        assert(ipc->generation == generation);++next_packet;
        if (!peek_quiesce) return -EAGAIN;
        *packet=(struct ph_ipc_packet){.operation=PH_LIFECYCLE_QUIESCE,
            .generation=generation,.correlation=99};return 0;
    }
    if (ring_mode) {
        assert(ipc->generation == generation);
        size_t step = next_packet++;
        if (!step || (step == 3 && ring_mode != 2)) {
            *packet = (struct ph_ipc_packet){.operation = 0x200, .generation = generation};
            return 0;
        }
        if ((ring_mode == 2 && step == 1) ||
            (ring_mode == 6 && step == 2) || step == 5) {
            *packet = (struct ph_ipc_packet){.operation = PH_LIFECYCLE_QUIESCE,
                .generation = generation, .correlation = 99};
            return 0;
        }
        assert(step < 5);
        return -EAGAIN;
    }
    assert(ipc->generation == generation && next_packet < 4);
    *packet = packets[next_packet++];
    return 0;
}

int ph_ipc_packet_release(struct ph_ipc_packet *packet) {
    assert(!packet->fd_count);
    memset(packet, 0, sizeof(*packet));
    return 0;
}

long ph_wait_readable(struct pacha_pollfd *descriptors, size_t count) {
    if (ring_mode == 10) {
        assert(pthread_equal(pthread_self(), receiver));
        assert(count == 2);
        assert(descriptors[0].fd == 30);
        pthread_mutex_lock(&lock);
        while (!atomic_load(&async_wakes) &&
            atomic_load(&async_consumed) >= 2 &&
            !(atomic_load(&async_stop_requested) && !atomic_load(&lifecycle.quiescing)))
            pthread_cond_wait(&changed, &lock);
        atomic_store(&async_wakes, 0);
        pthread_mutex_unlock(&lock);
        if (async_inject == 3 && atomic_load(&async_stop_requested)) return -EIO;
        return 1;
    }
    (void)descriptors;
    (void)count;
    if (ring_mode) {
        assert(!ring_pending && next_packet == 5);
        ++ring_waits;
        return 1;
    }
    abort(); /* All packets are already queued in the packet-only race. */
}

static int prepare(void *context, struct ph_ipc_packet *packet) {
    assert(pthread_equal(pthread_self(), receiver));
    assert(context == &lifecycle && packet->operation == 0x200);
    ++prepared;
    return failure == 1 ? -EIO : 0;
}

static int dispatch(void *context, void *service) {
    assert(!pthread_equal(pthread_self(), receiver));
    assert(context == &lifecycle && service == &task);
    ++dispatched;
    /* Duplicate notifications during work cannot claim the BUSY slot. */
    assert(!lifecycle.port.pending(&lifecycle));
    assert(!notify(0, KOBOX_LINUX_TASK_CONTROL_EVENT));
    assert(!notify(0, KOBOX_LINUX_TASK_CONTROL_EVENT));
    assert(!lifecycle.port.pending(&lifecycle));
    return failure == 2 ? -EIO : failure == 5 ? 7 : 0;
}

static int complete(void *context, struct ph_ipc *ipc) {
    assert(!pthread_equal(pthread_self(), receiver));
    assert(context == &lifecycle && ipc->generation == 9);
    ++completed;
    assert(dispatched == completed);
    assert(!lifecycle.port.pending(&lifecycle));
    assert(released + 1 == completed);
    return failure == 3 ? -EIO : 0;
}

static int release(void *context) {
    assert(!!pthread_equal(pthread_self(), receiver) ==
        (failure == 1 || ring_mode == 3));
    assert(context == &lifecycle);
    ++released;
    return failure == 4 ? -EIO : 0;
}

static void run_case(int injected) {
    memset(&lifecycle, 0, sizeof(lifecycle));
    next_packet = prepared = dispatched = completed = released = notifications = 0;
    failure = injected;
    ring_mode = 0;
    for (size_t index = 0; index < 4; ++index)
        packets[index] = (struct ph_ipc_packet){.operation = index == 2 ? PH_LIFECYCLE_QUIESCE : 0x200,
            .generation = 9, .correlation = index + 1};
    struct ph_ipc ipc = {.generation = 9, .fd = 16, .admitted = 1};
    struct ph_lifecycle_service service = {.context = &lifecycle, .prepare = prepare,
        .dispatch = dispatch, .complete = complete, .release = release};
    assert(!ph_lifecycle_start_service(&lifecycle, &ipc, &service));
    assert(!lifecycle.port.ready(&lifecycle));
    int result;
    for (;;) {
        pthread_mutex_lock(&lock);
        while (!(result = lifecycle.port.pending(&lifecycle))) pthread_cond_wait(&changed, &lock);
        pthread_mutex_unlock(&lock);
        if (result != 2) break;
        result = lifecycle.port.dispatch(&lifecycle, &task);
        if (injected == 3) assert(!result); /* Completion failure waits for cleanup. */
        if (result) break;
    }
    ph_lifecycle_finish(&lifecycle);
    if (!injected) {
        assert(result == 1 && prepared == 2 && dispatched == 2 && completed == 2 && released == 2);
        assert(next_packet == 3 && lifecycle.token == 3); /* Nothing admitted after STOP. */
        assert(lifecycle.port.dispatch(&lifecycle, &task) == -EPROTO); /* No duplicate execution. */
        assert(dispatched == 2);
    } else {
        assert(result == (injected == 5 ? -EPROTO : -EIO));
        assert(prepared == 1 && released == 1 && next_packet == 1);
        assert(dispatched == (injected == 1 ? 0u : 1u));
        assert(completed == (injected == 3 || injected == 4 ? 1u : 0u));
    }
}

static int ring_prepare(void *context, struct ph_ipc_packet *packet) {
    assert(context == &lifecycle && packet->operation == 0x200);
    if (!ring_packets++) ring_pending = 2;
    return PH_LIFECYCLE_SERVICE_IDLE;
}

static int ring_next(void *context) {
    assert(context == &lifecycle);
    if (ring_mode == 3 || (ring_mode == 7 && prepared == 1)) return -EIO;
    if (!ring_pending) return PH_LIFECYCLE_SERVICE_IDLE;
    /* Only the first request needs the native receiver handoff. */
    assert(!!pthread_equal(pthread_self(), receiver) == !prepared);
    --ring_pending;
    ++prepared;
    return 0;
}

static int ring_stop(void *context) {
    assert(context == &lifecycle);
    ++ring_stops;
    return ring_mode == 4 ? -EIO : 0;
}

static int ring_wait(void *context, int control_fd, uint64_t timeout) {
    assert(context == &lifecycle && pthread_equal(pthread_self(), receiver));
    assert(control_fd == 16 && timeout == PACHA_FD_WAIT_FOREVER);
    if (ring_mode == 9) { ++ring_waits; return -EIO; }
    struct pacha_pollfd control = {.fd = control_fd};
    assert(ph_wait_readable(&control, 1) == 1);
    return 0;
}

static void run_ring_case(int mode) {
    memset(&lifecycle, 0, sizeof(lifecycle));
    next_packet = prepared = dispatched = completed = released = notifications = 0;
    ring_pending = ring_packets = ring_stops = ring_waits = 0;
    failure = 0;
    ring_mode = mode;
    struct ph_ipc ipc = {.generation = 9, .fd = 16, .admitted = 1};
    struct ph_lifecycle_service service = {.context = &lifecycle, .prepare = ring_prepare,
        .dispatch = dispatch, .complete = complete, .release = release,
        .next = ring_next, .stop = ring_stop};
    if (mode == 1 || mode == 9) service.wait = ring_wait;
    assert(!ph_lifecycle_start_service(&lifecycle, &ipc, &service));
    if (mode == 5) {
        ph_lifecycle_finish(&lifecycle);
        assert(ring_stops == 1 && !next_packet && !dispatched);
        return;
    }
    assert(!lifecycle.port.ready(&lifecycle));
    int result;
    for (;;) {
        result = lifecycle.port.poll(&lifecycle);
        if (!result) {
            lifecycle.port.idle(&lifecycle);
            pthread_mutex_lock(&lock);
            while (!lifecycle.port.pending(&lifecycle)) pthread_cond_wait(&changed, &lock);
            pthread_mutex_unlock(&lock);
            continue;
        }
        if (result != 2) break;
        assert(!lifecycle.port.dispatch(&lifecycle, &task));
    }
    ph_lifecycle_finish(&lifecycle);
    assert(ring_stops == 1);
    if (mode == 9) {
        assert(result == -EIO && ring_waits == 1 && next_packet == 5);
        assert(prepared == 2 && dispatched == 2 && completed == 2 && released == 2);
    } else if (mode == 6) {
        assert(result == 1 && lifecycle.token == 99 && next_packet == 3);
        assert(prepared == 1 && dispatched == 1 && completed == 1 && released == 1);
        assert(ring_pending == 1); /* STOP preempts the next queued request. */
    } else if (mode == 7) {
        assert(result == -EIO && next_packet == 3);
        assert(prepared == 1 && dispatched == 1 && completed == 1 && released == 2);
    } else if (mode == 2) {
        assert(result == 1 && ring_pending == 2 && !dispatched && next_packet == 2);
    } else if (mode == 3) {
        assert(result == -EIO && !dispatched && released == 1 && next_packet == 2);
    } else {
        assert(result == (mode == 4 ? -EIO : 1));
        assert(prepared == 2 && dispatched == 2 && completed == 2 && released == 2);
        assert(ring_packets == 2 && ring_waits == 1 && next_packet == 6 && !ring_pending);
    }
}

static int peek_ready(void *context) {
    assert(context==&lifecycle);return peek_result;
}
static int peek_next(void *context) {
    assert(context==&lifecycle);++peek_next_calls;return 0;
}
static void run_peek_case(void) {
    memset(&lifecycle,0,sizeof(lifecycle));
    ring_mode=8;peek_result=peek_quiesce=0;peek_next_calls=0;next_packet=0;ring_stops=0;
    struct ph_ipc ipc={.generation=9,.fd=16,.admitted=1};
    struct ph_lifecycle_service service={.context=&lifecycle,.prepare=ring_prepare,
        .dispatch=dispatch,.complete=complete,.release=release,.stop=ring_stop,
        .next=peek_next,.poll_ready=peek_ready};
    assert(!ph_lifecycle_start_service(&lifecycle,&ipc,&service));
    /* Leave the native receiver at START; test its opening-task callbacks. */
    atomic_store(&lifecycle.work,2);
    for(unsigned i=0;i<1024;++i) assert(!lifecycle.port.poll(&lifecycle));
    assert(!next_packet && !peek_next_calls);
    peek_result=1;
    assert(lifecycle.port.poll(&lifecycle)==2);
    assert(next_packet==1 && peek_next_calls==1);
    atomic_store(&lifecycle.work,2);peek_quiesce=1;
    assert(!lifecycle.port.poll(&lifecycle));
    assert(lifecycle.token==99 && next_packet==2 && peek_next_calls==1);
    ph_lifecycle_finish(&lifecycle);assert(ring_stops==1);
    memset(&lifecycle,0,sizeof(lifecycle));peek_result=-EIO;peek_quiesce=0;next_packet=0;
    assert(!ph_lifecycle_start_service(&lifecycle,&ipc,&service));atomic_store(&lifecycle.work,2);
    assert(!lifecycle.port.poll(&lifecycle));
    assert(lifecycle.work_result==-EIO && !next_packet);
    ph_lifecycle_finish(&lifecycle);
}

static int async_prepare(void *context, struct ph_ipc_packet *packet) {
    assert(context == &lifecycle && !pthread_equal(pthread_self(), receiver));
    assert(packet->operation == 0x200 && packet->correlation == atomic_load(&async_jobs) + 1);
    atomic_store(&async_jobs, (unsigned)packet->correlation);
    return PH_LIFECYCLE_SERVICE_IDLE;
}
static int async_complete(void *context, struct ph_ipc *ipc) {
    (void)context; (void)ipc; abort(); /* Data completion belongs to advance. */
}
static int async_release(void *context) {
    (void)context; abort(); /* No synchronous staging slot exists. */
}
static int async_start(void *context, void *service) {
    assert(context == &lifecycle && service == &task);
    assert(!pthread_equal(pthread_self(), receiver) && !atomic_load(&async_started));
    atomic_store(&async_started, 1);
    return 0;
}
static int async_wake(void *context) {
    assert(context == &lifecycle && !pthread_equal(pthread_self(), receiver));
    atomic_store(&async_wakes, 1);
    atomic_fetch_add(&async_wake_calls, 1);
    async_changed();
    return 0;
}
static int async_advance(void *context, void *service, struct ph_ipc *ipc, bool quiescing) {
    assert(context == &lifecycle && service == &task && atomic_load(&async_started));
    assert(ipc->fd == 16);
    assert(!pthread_equal(pthread_self(), receiver));
    int progress = 0;
    unsigned jobs = atomic_load(&async_jobs);
    if (jobs != atomic_load(&async_started_jobs)) {
        atomic_store(&async_started_jobs, jobs);
        if (jobs >= 2) atomic_fetch_or(&async_done, 2);
        progress = PH_LIFECYCLE_SERVICE_PROGRESS;
    }
    if (atomic_load(&async_unblock) && jobs && !(atomic_load(&async_done) & 1)) {
        atomic_fetch_or(&async_done, 1);
        progress = PH_LIFECYCLE_SERVICE_PROGRESS;
    }
    unsigned done = atomic_load(&async_done);
    unsigned returned = atomic_load(&async_returned);
    if (done != returned) {
        /* Transport is single-owner; completion 2 precedes blocked request 1. */
        if (!(returned & 2)) assert(done & 2);
        atomic_store(&async_returned, done);
        atomic_store(&async_freed, done);
        progress = PH_LIFECYCLE_SERVICE_PROGRESS;
    }
    if (quiescing && atomic_load(&async_freed) == 3)
        return PH_LIFECYCLE_SERVICE_IDLE;
    if (async_inject == 1 && !quiescing && atomic_load(&async_stop_requested))
        return -EIO;
    return progress;
}
static int async_stop(void *context) {
    assert(context == &lifecycle && !pthread_equal(pthread_self(), receiver));
    assert(atomic_load(&async_freed) == 3 && atomic_load(&async_finished));
    ++ring_stops;
    return 0;
}
static int async_finish(void *context, void *service) {
    assert(context == &lifecycle && service == &task && !ring_stops);
    assert(!pthread_equal(pthread_self(), receiver) && atomic_load(&async_freed) == 3);
    atomic_store(&async_finished, 1);
    return 0;
}
static void async_pump_until(atomic_uint *word, unsigned value) {
    for (unsigned tries = 0; tries < 100000 && atomic_load(word) != value; ++tries) {
        assert(lifecycle.port.advance(&lifecycle, &task) >= 0);
        sched_yield();
    }
    assert(atomic_load(word) == value);
}
static void run_async_case(int inject) {
    memset(&lifecycle, 0, sizeof(lifecycle));
    ring_mode = 10; next_packet = ring_stops = 0; async_inject = inject;
    atomic_store(&async_jobs, 0); atomic_store(&async_started_jobs, 0);
    atomic_store(&async_consumed, 0); atomic_store(&async_wake_calls, 0);
    atomic_store(&async_done, 0); atomic_store(&async_returned, 0); atomic_store(&async_freed, 0);
    atomic_store(&async_stop_requested, 0); atomic_store(&async_unblock, 0);
    atomic_store(&async_wakes, 0); atomic_store(&async_started, 0); atomic_store(&async_finished, 0);
    struct ph_ipc ipc = {.generation = 9, .fd = 16, .admitted = 1};
    struct ph_lifecycle_service service = {.context = &lifecycle,
        .prepare = async_prepare, .complete = async_complete, .release = async_release,
        .start = async_start, .advance = async_advance, .finish = async_finish,
        .wake = async_wake, .wake_fd = 30, .stop = async_stop};
    struct ph_lifecycle_service invalid = service;
    invalid.finish = NULL;
    assert(ph_lifecycle_start_service(&lifecycle, &ipc, &invalid) == -EINVAL);
    assert(!ph_lifecycle_start_service(&lifecycle, &ipc, &service));
    assert(!lifecycle.port.dispatch && !lifecycle.port.poll && !lifecycle.port.idle);
    assert(!lifecycle.port.start(&lifecycle, &task));
    assert(!lifecycle.port.ready(&lifecycle));
    async_pump_until(&async_freed, 2);
    assert(!atomic_load(&async_wake_calls)); /* No completion-to-native IPC. */
    assert(atomic_load(&async_returned) == 2 && !ring_stops && !atomic_load(&async_finished));
    atomic_store(&async_stop_requested, 1); async_changed();
    async_pump_until(&lifecycle.quiescing, 1);
    assert(!atomic_load(&lifecycle.terminal)); /* STOP cannot unmap a blocked worker's lease. */
    assert(!ring_stops && !atomic_load(&async_finished));
    int expected = inject == 2 ? -EPIPE : inject == 4 ? -EPROTO : inject ? -EIO : 1;
    if (inject) assert(atomic_load(&lifecycle.async_error) == expected);
    atomic_store(&async_unblock, 1);
    async_pump_until(&async_freed, 3);
    for (unsigned tries = 0; !atomic_load(&lifecycle.terminal) && tries < 100000; ++tries) {
        assert(lifecycle.port.advance(&lifecycle, &task) >= 0);
        sched_yield();
    }
    assert(lifecycle.port.pending(&lifecycle) == expected);
    assert(!lifecycle.port.finish(&lifecycle, &task));
    ph_lifecycle_finish(&lifecycle);
    assert(atomic_load(&async_finished) && ring_stops == 1);
    assert(next_packet == (!inject || inject == 4 ? 3u : 2u));
}

static int startup_start(void *context, void *service) {
    assert(context == &lifecycle && service == &task);
    assert(!pthread_equal(pthread_self(), receiver));
    if (startup_failure) return -EIO; /* start rolled back its Linux resources. */
    ++startup_started;
    return 0;
}
static int startup_finish(void *context, void *service) {
    assert(context == &lifecycle && service == &task && startup_started == 1);
    assert(!pthread_equal(pthread_self(), receiver) && !startup_stopped);
    ++startup_finished;
    return 0;
}
static int startup_stop(void *context) {
    assert(context == &lifecycle && !pthread_equal(pthread_self(), receiver));
    assert(!startup_stopped);
    ++startup_stopped;
    return 0;
}
static void run_async_startup_failure(int stage) {
    memset(&lifecycle, 0, sizeof(lifecycle));
    ring_mode = 10;
    ready_failure = stage == 2 ? -EIO : 0;
    startup_failure = stage == 1;
    startup_started = startup_finished = startup_stopped = 0;
    struct ph_ipc ipc = {.generation = 9, .fd = 16, .admitted = 1};
    struct ph_lifecycle_service service = {.context = &lifecycle,
        .prepare = async_prepare, .complete = async_complete, .release = async_release,
        .start = startup_start, .advance = async_advance, .finish = startup_finish,
        .stop = startup_stop, .wake = async_wake, .wake_fd = 30};
    assert(!ph_lifecycle_start_service(&lifecycle, &ipc, &service));
    if (stage) assert(lifecycle.port.start(&lifecycle, &task) == (stage == 1 ? -EIO : 0));
    if (stage == 2) {
        assert(lifecycle.port.ready(&lifecycle) == -EIO);
        assert(!lifecycle.port.finish(&lifecycle, &task));
    }
    ph_lifecycle_finish(&lifecycle);
    assert(startup_stopped == 1 && startup_finished == (stage == 2));
    assert(!lifecycle.receiver);
    ready_failure = 0;
}
int main(void) {
    for (int repeat = 0; repeat < 32; ++repeat)
        for (int injected = 0; injected <= 5; ++injected) run_case(injected);
    for (int repeat = 0; repeat < 32; ++repeat)
        for (int mode = 1; mode <= 7; ++mode) run_ring_case(mode);
    run_peek_case();
    run_ring_case(9);
    for (unsigned repeat = 0; repeat < 32; ++repeat) {
        for (int inject = 0; inject <= 4; ++inject) run_async_case(inject);
        for (int stage = 0; stage <= 2; ++stage) run_async_startup_failure(stage);
    }
    puts("native lifecycle: PASS legacy dispatch, async single owner, out-of-order completion, STOP drain, failure cleanup");
    return 0;
}
