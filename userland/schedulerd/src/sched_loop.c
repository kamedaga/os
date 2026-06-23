#include "sched_loop.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static uint64_t event_weight(const pacha_sched_event_t *event)
{
    return event->weight == 0 ? PACHA_EEVDF_DEFAULT_WEIGHT : event->weight;
}

static uint64_t event_slice(const pacha_sched_event_t *event)
{
    return event->slice_ns == 0 ? PACHA_EEVDF_DEFAULT_SLICE_NS : event->slice_ns;
}

static uint32_t event_cpu(const pacha_sched_loop_t *loop, const pacha_sched_event_t *event)
{
    if (loop == 0 || event == 0 || loop->cpu_count == 0) return 0;
    return event->cpu_id < loop->cpu_count ? event->cpu_id : 0;
}

static int event_i64(uint64_t value, int64_t *out)
{
    if (out == 0 || value > (uint64_t)INT64_MAX) return 0;
    *out = (int64_t)value;
    return 1;
}

static int event_positive_i64(uint64_t value, int64_t fallback, int64_t *out)
{
    uint64_t selected = value == 0 ? (uint64_t)fallback : value;
    if (!event_i64(selected, out)) return 0;
    return *out > 0;
}

static int commit_decision(
    pacha_sched_loop_t *loop,
    const pacha_sched_decision *decision,
    uint64_t sequence)
{
    if (loop == 0 || decision == 0) return -1;
    if (decision->kind == PACHA_SCHED_DECISION_NONE) return 0;

    pacha_sched_commit_t commit;
    memset(&commit, 0, sizeof(commit));
    commit.size = sizeof(commit);
    commit.version = PACHA_SCHED_ABI_VERSION;
    commit.sequence = sequence;

    switch (decision->kind) {
    case PACHA_SCHED_DECISION_RUN_THREAD:
        if (decision->cpu_id >= loop->cpu_count) return -2;
        if (decision->thread_id <= 0 || decision->generation < 0) return -3;
        commit.cpu_id = (uint32_t)decision->cpu_id;
        commit.thread_id = (uint64_t)decision->thread_id;
        commit.generation = (uint64_t)decision->generation;
        break;
    case PACHA_SCHED_DECISION_IDLE:
        if (decision->cpu_id >= loop->cpu_count) return -4;
        commit.cpu_id = (uint32_t)decision->cpu_id;
        commit.thread_id = PACHA_SCHED_NO_THREAD;
        commit.generation = 0;
        break;
    case PACHA_SCHED_DECISION_NONE:
    default:
        return -5;
    }

    if (loop->schedctl_fd >= 0 &&
        ioctl(loop->schedctl_fd, PACHA_SCHED_IOCTL_COMMIT, &commit) != 0) {
        return -6;
    }
    loop->commit_count++;
    return 0;
}

static int pick_and_commit(pacha_sched_loop_t *loop, uint32_t cpu, uint64_t sequence)
{
    if (loop == 0 || cpu >= loop->cpu_count) return -1;
    pacha_sched_rc rc = pacha_sched_pick(
        &loop->sched,
        cpu,
        &loop->decision,
        &loop->pick_scratch,
        &loop->runqueue_scratch);
    if (rc != PACHA_SCHED_OK) return -10 - (int)rc;
    return commit_decision(loop, &loop->decision, sequence);
}

static int add_or_wake(pacha_sched_loop_t *loop, const pacha_sched_event_t *event)
{
    int64_t thread_id;
    int64_t generation;
    int64_t weight;
    int64_t slice_ns;
    if (!event_i64(event->thread_id, &thread_id)) return -1;
    if (!event_i64(event->generation, &generation)) return -2;
    if (!event_positive_i64(event_weight(event), PACHA_EEVDF_DEFAULT_WEIGHT, &weight)) return -3;
    if (!event_positive_i64(event_slice(event), PACHA_EEVDF_DEFAULT_SLICE_NS, &slice_ns)) return -4;

    pacha_sched_rc rc = pacha_sched_add_thread(
        &loop->sched,
        thread_id,
        generation,
        weight,
        slice_ns,
        &loop->decision,
        &loop->runqueue_scratch);
    if (rc == PACHA_SCHED_OK) return 0;

    rc = pacha_sched_wake_thread(
        &loop->sched,
        thread_id,
        &loop->decision,
        &loop->runqueue_scratch);
    if (rc == PACHA_SCHED_OK || rc == PACHA_SCHED_ERR_STATE) return 0;
    return -10 - (int)rc;
}

static int block_thread(pacha_sched_loop_t *loop, uint64_t raw_thread_id)
{
    int64_t thread_id;
    if (!event_i64(raw_thread_id, &thread_id)) return -1;
    pacha_sched_rc rc = pacha_sched_block_thread(
        &loop->sched,
        thread_id,
        &loop->decision,
        &loop->runqueue_scratch);
    if (rc == PACHA_SCHED_OK ||
        rc == PACHA_SCHED_ERR_INVALID ||
        rc == PACHA_SCHED_ERR_STATE) {
        return 0;
    }
    return -10 - (int)rc;
}

static int exit_thread(pacha_sched_loop_t *loop, uint64_t raw_thread_id)
{
    int64_t thread_id;
    if (!event_i64(raw_thread_id, &thread_id)) return -1;
    pacha_sched_rc rc = pacha_sched_exit_thread(
        &loop->sched,
        thread_id,
        &loop->decision,
        &loop->runqueue_scratch);
    if (rc == PACHA_SCHED_OK ||
        rc == PACHA_SCHED_ERR_INVALID ||
        rc == PACHA_SCHED_ERR_STATE) {
        return 0;
    }
    return -10 - (int)rc;
}

static int finish_cpu(pacha_sched_loop_t *loop, uint32_t cpu)
{
    pacha_sched_rc rc = pacha_sched_finish_current(
        &loop->sched,
        cpu,
        &loop->decision,
        &loop->runqueue_scratch);
    if (rc == PACHA_SCHED_OK || rc == PACHA_SCHED_ERR_STATE) return 0;
    return -10 - (int)rc;
}

static int charge_and_finish(pacha_sched_loop_t *loop, const pacha_sched_event_t *event)
{
    const uint32_t cpu = event_cpu(loop, event);
    if (event->runtime_ns > (uint64_t)INT64_MAX) return -1;
    pacha_sched_rc rc = pacha_sched_on_timer(
        &loop->sched,
        cpu,
        (int64_t)event->runtime_ns,
        &loop->decision,
        &loop->runqueue_scratch);
    if (rc != PACHA_SCHED_OK) return -10 - (int)rc;
    return finish_cpu(loop, cpu);
}

void pacha_sched_loop_init(pacha_sched_loop_t *loop, int schedctl_fd, int event_fd)
{
    memset(loop, 0, sizeof(*loop));
    loop->schedctl_fd = schedctl_fd;
    loop->event_fd = event_fd;
    loop->cpu_count = PACHA_SCHED_LOOP_MAX_CPUS;
    pacha_sched_empty_state(loop->cpu_count, &loop->sched);
}

int pacha_sched_loop_dispatch(pacha_sched_loop_t *loop, const pacha_sched_event_t *event)
{
    if (loop == 0 || event == 0) return -1;
    if (event->size < sizeof(*event) || event->version != PACHA_SCHED_ABI_VERSION) return -2;
    loop->dispatch_count++;
    const uint32_t cpu = event_cpu(loop, event);

    switch (event->type) {
    case PACHA_SCHED_EVENT_THREAD_READY:
        if (event->thread_id == PACHA_SCHED_NO_THREAD) return -3;
        {
            int status = add_or_wake(loop, event);
            if (status != 0) return status;
        }
        return pick_and_commit(loop, cpu, event->sequence);
    case PACHA_SCHED_EVENT_THREAD_BLOCKED:
        if (event->thread_id == PACHA_SCHED_NO_THREAD) return -3;
        {
            int status = block_thread(loop, event->thread_id);
            if (status != 0) return status;
        }
        return pick_and_commit(loop, cpu, event->sequence);
    case PACHA_SCHED_EVENT_THREAD_EXITED:
        if (event->thread_id == PACHA_SCHED_NO_THREAD) return -3;
        {
            int status = exit_thread(loop, event->thread_id);
            if (status != 0) return status;
        }
        return pick_and_commit(loop, cpu, event->sequence);
    case PACHA_SCHED_EVENT_THREAD_YIELD:
    case PACHA_SCHED_EVENT_TICK:
        if (event->thread_id != PACHA_SCHED_NO_THREAD) {
            int status = charge_and_finish(loop, event);
            if (status != 0) return status;
        } else {
            int status = finish_cpu(loop, cpu);
            if (status != 0) return status;
        }
        return pick_and_commit(loop, cpu, event->sequence);
    case PACHA_SCHED_EVENT_CPU_IDLE:
        return pick_and_commit(loop, cpu, event->sequence);
    default:
        return -4;
    }
}

int pacha_sched_loop_run_once(pacha_sched_loop_t *loop)
{
    if (loop == 0 || loop->event_fd < 0) return -1;
    pacha_sched_event_t event;
    ssize_t nread = read(loop->event_fd, &event, sizeof(event));
    if (nread < 0) {
        return errno == EAGAIN || errno == EWOULDBLOCK ? -5 : -6;
    }
    if (nread >= 0 && nread < (ssize_t)sizeof(event)) return -5;
    if (nread != (ssize_t)sizeof(event)) return -3;
    return pacha_sched_loop_dispatch(loop, &event);
}

int pacha_sched_loop_demo(pacha_sched_loop_t *loop)
{
    static const pacha_sched_event_t events[] = {
        {
            .size = sizeof(pacha_sched_event_t),
            .version = PACHA_SCHED_ABI_VERSION,
            .type = PACHA_SCHED_EVENT_THREAD_READY,
            .sequence = 1,
            .cpu_id = 0,
            .thread_id = 1,
            .generation = 1,
            .weight = 1024,
            .slice_ns = 3000000,
        },
        {
            .size = sizeof(pacha_sched_event_t),
            .version = PACHA_SCHED_ABI_VERSION,
            .type = PACHA_SCHED_EVENT_THREAD_READY,
            .sequence = 2,
            .cpu_id = 0,
            .thread_id = 2,
            .generation = 1,
            .weight = 2048,
            .slice_ns = 3000000,
        },
        {
            .size = sizeof(pacha_sched_event_t),
            .version = PACHA_SCHED_ABI_VERSION,
            .type = PACHA_SCHED_EVENT_CPU_IDLE,
            .sequence = 3,
            .cpu_id = 0,
        },
        {
            .size = sizeof(pacha_sched_event_t),
            .version = PACHA_SCHED_ABI_VERSION,
            .type = PACHA_SCHED_EVENT_TICK,
            .sequence = 4,
            .cpu_id = 0,
            .thread_id = 1,
            .generation = 1,
            .runtime_ns = 750000,
        },
    };
    for (unsigned i = 0; i < sizeof(events) / sizeof(events[0]); i++) {
        int status = pacha_sched_loop_dispatch(loop, &events[i]);
        if (status != 0) return status;
    }
    return 0;
}
