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
    return 0;
}

int ph_ipc_receive(struct ph_ipc *ipc, uint64_t generation, struct ph_ipc_packet *packet) {
    if (ring_mode) {
        assert(ipc->generation == generation);
        size_t step = next_packet++;
        if (!step || (step == 3 && ring_mode != 2)) {
            *packet = (struct ph_ipc_packet){.operation = 0x200, .generation = generation};
            return 0;
        }
        if ((ring_mode == 2 && step == 1) || step == 5) {
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
    (void)descriptors;
    (void)count;
    if (ring_mode) {
        assert(!ring_pending && next_packet == 5);
        ++ring_waits;
        return 1;
    }
    abort(); /* All packets are already queued in the packet-only race. */
}

static int prepare(void *context, const struct ph_ipc_packet *packet) {
    assert(context == &lifecycle && packet->operation == 0x200);
    ++prepared;
    return failure == 1 ? -EIO : 0;
}

static int dispatch(void *context, void *service) {
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
    assert(context == &lifecycle && ipc->generation == 9);
    ++completed;
    assert(dispatched == completed);
    return failure == 3 ? -EIO : 0;
}

static int release(void *context) {
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

static int ring_prepare(void *context, const struct ph_ipc_packet *packet) {
    assert(context == &lifecycle && packet->operation == 0x200);
    if (!ring_packets++) ring_pending = 2;
    return PH_LIFECYCLE_SERVICE_IDLE;
}

static int ring_next(void *context) {
    assert(context == &lifecycle);
    if (ring_mode == 3) return -EIO;
    if (!ring_pending) return PH_LIFECYCLE_SERVICE_IDLE;
    --ring_pending;
    ++prepared;
    return 0;
}

static int ring_stop(void *context) {
    assert(context == &lifecycle);
    ++ring_stops;
    return ring_mode == 4 ? -EIO : 0;
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
    assert(!ph_lifecycle_start_service(&lifecycle, &ipc, &service));
    if (mode == 5) {
        ph_lifecycle_finish(&lifecycle);
        assert(ring_stops == 1 && !next_packet && !dispatched);
        return;
    }
    assert(!lifecycle.port.ready(&lifecycle));
    int result;
    for (;;) {
        pthread_mutex_lock(&lock);
        while (!(result = lifecycle.port.pending(&lifecycle))) pthread_cond_wait(&changed, &lock);
        pthread_mutex_unlock(&lock);
        if (result != 2) break;
        assert(!lifecycle.port.dispatch(&lifecycle, &task));
    }
    ph_lifecycle_finish(&lifecycle);
    assert(ring_stops == 1);
    if (mode == 2) {
        assert(result == 1 && ring_pending == 2 && !dispatched && next_packet == 2);
    } else if (mode == 3) {
        assert(result == -EIO && !dispatched && released == 1 && next_packet == 2);
    } else {
        assert(result == (mode == 4 ? -EIO : 1));
        assert(prepared == 2 && dispatched == 2 && completed == 2 && released == 2);
        assert(ring_packets == 2 && ring_waits == 1 && next_packet == 6 && !ring_pending);
    }
}

int main(void) {
    for (int repeat = 0; repeat < 32; ++repeat)
        for (int injected = 0; injected <= 5; ++injected) run_case(injected);
    for (int repeat = 0; repeat < 32; ++repeat)
        for (int mode = 1; mode <= 5; ++mode) run_ring_case(mode);
    puts("native lifecycle dispatch: PASS publication, duplicate notifications, STOP ordering, failure cleanup");
    return 0;
}
