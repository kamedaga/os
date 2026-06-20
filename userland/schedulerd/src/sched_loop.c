#include "sched_loop.h"

#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void debug_log(const char *label, const pacha_sched_event_t *event, uint32_t cpu, uint64_t thread_id, int status)
{
    (void)label;
    (void)event;
    (void)cpu;
    (void)thread_id;
    (void)status;
}

static void debug_mark(const char *message)
{
    (void)message;
}

static uint64_t event_weight(const pacha_sched_event_t *event)
{
    return event->weight == 0 ? PACHA_EEVDF_DEFAULT_WEIGHT : event->weight;
}

static uint64_t event_slice(const pacha_sched_event_t *event)
{
    return event->slice_ns == 0 ? PACHA_EEVDF_DEFAULT_SLICE_NS : event->slice_ns;
}

static uint32_t event_cpu(const pacha_sched_event_t *event)
{
    return event->cpu_id < PACHA_SCHED_LOOP_MAX_CPUS ? event->cpu_id : 0;
}

static void note_cpu(pacha_sched_loop_t *loop, uint32_t cpu)
{
    if (cpu < PACHA_SCHED_LOOP_MAX_CPUS) {
        loop->known_cpu_mask |= 1ull << cpu;
    }
}

static uint32_t choose_ready_cpu(pacha_sched_loop_t *loop, uint32_t preferred)
{
    if (preferred >= PACHA_SCHED_LOOP_MAX_CPUS) preferred = 0;
    note_cpu(loop, preferred);
    uint64_t idle = loop->idle_cpu_mask & loop->known_cpu_mask;
    if (idle != 0) {
        for (uint32_t cpu = 0; cpu < PACHA_SCHED_LOOP_MAX_CPUS; cpu++) {
            if ((idle & (1ull << cpu)) != 0) return cpu;
        }
    }
    return preferred;
}

static uint32_t first_cpu_in_mask(uint64_t mask)
{
    for (uint32_t cpu = 1; cpu < PACHA_SCHED_LOOP_MAX_CPUS; cpu++) {
        if ((mask & (1ull << cpu)) != 0) return cpu;
    }
    return PACHA_SCHED_LOOP_MAX_CPUS;
}

static uint32_t running_cpu_for(
    const pacha_sched_loop_t *loop,
    uint64_t thread_id,
    uint64_t generation)
{
    if (loop == 0 || thread_id == PACHA_SCHED_NO_THREAD) {
        return PACHA_SCHED_LOOP_MAX_CPUS;
    }
    for (uint32_t cpu = 0; cpu < PACHA_SCHED_LOOP_MAX_CPUS; cpu++) {
        if (loop->running_thread[cpu] == thread_id &&
            loop->running_generation[cpu] == generation) {
            return cpu;
        }
    }
    return PACHA_SCHED_LOOP_MAX_CPUS;
}

static void clear_running_cpu(pacha_sched_loop_t *loop, uint32_t cpu)
{
    if (loop == 0 || cpu >= PACHA_SCHED_LOOP_MAX_CPUS) return;
    loop->running_thread[cpu] = PACHA_SCHED_NO_THREAD;
    loop->running_generation[cpu] = 0;
}

static void note_running_cpu(
    pacha_sched_loop_t *loop,
    uint32_t cpu,
    uint64_t thread_id,
    uint64_t generation)
{
    if (loop == 0 || cpu >= PACHA_SCHED_LOOP_MAX_CPUS) return;
    loop->running_thread[cpu] = thread_id;
    loop->running_generation[cpu] = generation;
}

static pacha_eevdf_entity_t *find_entity_on_cpu(pacha_sched_loop_t *loop, uint64_t thread_id, uint32_t *cpu_out)
{
    for (uint32_t cpu = 0; cpu < PACHA_SCHED_LOOP_MAX_CPUS; cpu++) {
        pacha_eevdf_entity_t *entity = pacha_eevdf_find(&loop->runqueues[cpu], thread_id);
        if (entity == 0) continue;
        if (entity->state == PACHA_EEVDF_EMPTY || entity->state == PACHA_EEVDF_EXITED) continue;
        if (cpu_out != 0) *cpu_out = cpu;
        return entity;
    }
    return 0;
}

static int ensure_entity_on_cpu(pacha_sched_loop_t *loop, const pacha_sched_event_t *event, uint32_t cpu)
{
    const uint32_t running_cpu = running_cpu_for(loop, event->thread_id, event->generation);
    if (running_cpu != PACHA_SCHED_LOOP_MAX_CPUS && running_cpu != cpu) {
        return -6;
    }
    uint32_t existing_cpu = 0;
    pacha_eevdf_entity_t *entity = find_entity_on_cpu(loop, event->thread_id, &existing_cpu);
    if (entity != 0) {
        if (existing_cpu != cpu) {
            (void)pacha_eevdf_exit(&loop->runqueues[existing_cpu], event->thread_id);
            return pacha_eevdf_reset(
                &loop->runqueues[cpu],
                event->thread_id,
                event->generation,
                event_weight(event),
                event_slice(event));
        }
        if (entity->generation != event->generation || entity->state == PACHA_EEVDF_EXITED) {
            return pacha_eevdf_reset(
                &loop->runqueues[cpu],
                event->thread_id,
                event->generation,
                event_weight(event),
                event_slice(event));
        }
        entity->generation = event->generation;
        entity->weight = event_weight(event);
        entity->slice_ns = event_slice(event);
        return pacha_eevdf_wake(&loop->runqueues[cpu], event->thread_id);
    }
    return pacha_eevdf_reset(
        &loop->runqueues[cpu],
        event->thread_id,
        event->generation,
        event_weight(event),
        event_slice(event));
}

static int charge_running(pacha_sched_loop_t *loop, const pacha_sched_event_t *event)
{
    uint32_t cpu = event_cpu(event);
    pacha_eevdf_entity_t *entity = pacha_eevdf_find(&loop->runqueues[cpu], event->thread_id);
    if (entity == 0 || entity->generation != event->generation || entity->state == PACHA_EEVDF_EXITED) {
        const uint32_t running_cpu = running_cpu_for(loop, event->thread_id, event->generation);
        if (running_cpu != PACHA_SCHED_LOOP_MAX_CPUS && running_cpu != cpu) {
            return -5;
        }
        int status = ensure_entity_on_cpu(loop, event, cpu);
        if (status != 0) return status;
        entity = pacha_eevdf_find(&loop->runqueues[cpu], event->thread_id);
        if (entity == 0) return -1;
    }
    if (entity->state == PACHA_EEVDF_RUNNABLE) {
        if (pacha_eevdf_mark_running(&loop->runqueues[cpu], event->thread_id) != 0) return -2;
    }
    if (event->runtime_ns != 0) {
        if (pacha_eevdf_charge(&loop->runqueues[cpu], event->thread_id, event->runtime_ns) != 0) return -3;
    }
    if (entity->state == PACHA_EEVDF_RUNNING) {
        if (pacha_eevdf_requeue_running(&loop->runqueues[cpu], event->thread_id) != 0) return -4;
    }
    return 0;
}

static int move_pick_to_commit_cpu(
    pacha_sched_loop_t *loop,
    uint32_t commit_cpu,
    uint32_t *pick_cpu,
    pacha_eevdf_entity_t **picked)
{
    if (loop == 0 || pick_cpu == 0 || picked == 0 || *picked == 0) return -1;
    if (commit_cpu >= PACHA_SCHED_LOOP_MAX_CPUS || *pick_cpu >= PACHA_SCHED_LOOP_MAX_CPUS) return -2;
    if (*pick_cpu == commit_cpu) return 0;

    pacha_eevdf_entity_t snapshot = **picked;
    snapshot.state = PACHA_EEVDF_RUNNABLE;
    if (pacha_eevdf_exit(&loop->runqueues[*pick_cpu], snapshot.thread_id) != 0) return -3;
    if (pacha_eevdf_import_runnable(&loop->runqueues[commit_cpu], &snapshot) != 0) return -4;

    *pick_cpu = commit_cpu;
    *picked = pacha_eevdf_find(&loop->runqueues[commit_cpu], snapshot.thread_id);
    return *picked == 0 ? -5 : 0;
}

static int submit_pick(
    pacha_sched_loop_t *loop,
    uint32_t commit_cpu,
    uint32_t *pick_cpu,
    pacha_eevdf_entity_t **picked,
    uint64_t sequence)
{
    if (loop == 0 || pick_cpu == 0 || picked == 0) return -1;
    if (*picked != 0 && move_pick_to_commit_cpu(loop, commit_cpu, pick_cpu, picked) != 0) {
        return -2;
    }

    pacha_sched_commit_t commit;
    memset(&commit, 0, sizeof(commit));
    commit.size = sizeof(commit);
    commit.version = PACHA_SCHED_ABI_VERSION;
    commit.cpu_id = commit_cpu;
    commit.thread_id = *picked == 0 ? PACHA_SCHED_NO_THREAD : (*picked)->thread_id;
    commit.generation = *picked == 0 ? 0 : (*picked)->generation;
    commit.sequence = sequence;

    if (*picked != 0) {
        const uint32_t running_cpu = running_cpu_for(loop, commit.thread_id, commit.generation);
        if (running_cpu != PACHA_SCHED_LOOP_MAX_CPUS && running_cpu != commit_cpu) {
            debug_log("commit-running", 0, commit_cpu, commit.thread_id, -5);
            return -5;
        }
    }

    if (loop->schedctl_fd >= 0 && ioctl(loop->schedctl_fd, PACHA_SCHED_IOCTL_COMMIT, &commit) != 0) {
        debug_log("commit-failed", 0, commit_cpu, commit.thread_id, -3);
        return -3;
    }
    if (*picked != 0) {
        if (pacha_eevdf_mark_running(&loop->runqueues[*pick_cpu], (*picked)->thread_id) != 0) return -4;
        note_running_cpu(loop, commit_cpu, (*picked)->thread_id, (*picked)->generation);
        if (commit_cpu < PACHA_SCHED_LOOP_MAX_CPUS) loop->idle_cpu_mask &= ~(1ull << commit_cpu);
    } else {
        clear_running_cpu(loop, commit_cpu);
    }
    loop->commit_count++;
    debug_log("commit", 0, commit_cpu, commit.thread_id, 0);
    return 0;
}

static int commit_pick(pacha_sched_loop_t *loop, uint32_t cpu_id, uint64_t sequence)
{
    uint32_t target_cpu = cpu_id < PACHA_SCHED_LOOP_MAX_CPUS ? cpu_id : 0;
    note_cpu(loop, target_cpu);

    uint64_t eligible_mask = loop->idle_cpu_mask & loop->known_cpu_mask & ~1ull;
    if (eligible_mask == 0) return 0;

    for (unsigned attempt = 0; attempt < PACHA_SCHED_LOOP_MAX_CPUS; attempt++) {
        uint32_t commit_cpu = ((eligible_mask & (1ull << target_cpu)) != 0)
            ? target_cpu
            : first_cpu_in_mask(eligible_mask);
        if (commit_cpu >= PACHA_SCHED_LOOP_MAX_CPUS) return 0;

        uint32_t pick_cpu = commit_cpu;
        pacha_eevdf_entity_t *picked = pacha_eevdf_pick(&loop->runqueues[pick_cpu]);
        for (uint32_t cpu = 0; picked == 0 && cpu < PACHA_SCHED_LOOP_MAX_CPUS; cpu++) {
            if (cpu == pick_cpu) continue;
            picked = pacha_eevdf_pick(&loop->runqueues[cpu]);
            if (picked != 0) pick_cpu = cpu;
        }
        if (picked == 0) {
            debug_log("no-pick", 0, commit_cpu, 0, 0);
            return 0;
        }

        if (submit_pick(loop, commit_cpu, &pick_cpu, &picked, sequence) == 0) {
            return 0;
        }

        eligible_mask &= ~(1ull << commit_cpu);
    }
    return 0;
}

void pacha_sched_loop_init(pacha_sched_loop_t *loop, int schedctl_fd, int event_fd)
{
    memset(loop, 0, sizeof(*loop));
    for (uint32_t cpu = 0; cpu < PACHA_SCHED_LOOP_MAX_CPUS; cpu++) {
        pacha_eevdf_init(&loop->runqueues[cpu]);
    }
    loop->schedctl_fd = schedctl_fd;
    loop->event_fd = event_fd;
    loop->known_cpu_mask = 1ull;
}

int pacha_sched_loop_dispatch(pacha_sched_loop_t *loop, const pacha_sched_event_t *event)
{
    if (loop == 0 || event == 0) return -1;
    if (event->size < sizeof(*event) || event->version != PACHA_SCHED_ABI_VERSION) return -2;
    loop->dispatch_count++;
    const uint32_t cpu = event_cpu(event);
    note_cpu(loop, cpu);
    debug_log("event", event, cpu, event->thread_id, 0);

    switch (event->type) {
    case PACHA_SCHED_EVENT_THREAD_READY:
        if (event->thread_id == PACHA_SCHED_NO_THREAD) return -3;
        {
            uint32_t target_cpu = choose_ready_cpu(loop, cpu);
            debug_log("ready-target", event, target_cpu, event->thread_id, 0);
            int status = ensure_entity_on_cpu(loop, event, target_cpu);
            if (status != 0) {
                debug_log("ready-ensure-failed", event, target_cpu, event->thread_id, status);
                return status;
            }
            if ((loop->idle_cpu_mask & (1ull << target_cpu)) != 0) {
                return commit_pick(loop, target_cpu, event->sequence);
            }
        }
        return commit_pick(loop, cpu, event->sequence);
    case PACHA_SCHED_EVENT_THREAD_BLOCKED:
        if (event->thread_id == PACHA_SCHED_NO_THREAD) return -3;
        loop->idle_cpu_mask |= 1ull << cpu;
        if (loop->running_thread[cpu] == event->thread_id &&
            loop->running_generation[cpu] == event->generation) {
            clear_running_cpu(loop, cpu);
        }
        {
            uint32_t entity_cpu = cpu;
            pacha_eevdf_entity_t *entity = find_entity_on_cpu(loop, event->thread_id, &entity_cpu);
            if (entity != 0 && entity->generation == event->generation) {
                int status = pacha_eevdf_block(&loop->runqueues[entity_cpu], event->thread_id);
                if (status != 0) return status;
            }
        }
        return commit_pick(loop, cpu, event->sequence);
    case PACHA_SCHED_EVENT_THREAD_EXITED:
        if (event->thread_id == PACHA_SCHED_NO_THREAD) return -3;
        loop->idle_cpu_mask |= 1ull << cpu;
        if (loop->running_thread[cpu] == event->thread_id &&
            loop->running_generation[cpu] == event->generation) {
            clear_running_cpu(loop, cpu);
        }
        {
            uint32_t entity_cpu = cpu;
            pacha_eevdf_entity_t *entity = find_entity_on_cpu(loop, event->thread_id, &entity_cpu);
            if (entity != 0 && entity->generation == event->generation) {
                int status = pacha_eevdf_exit(&loop->runqueues[entity_cpu], event->thread_id);
                if (status != 0) return status;
            }
        }
        return commit_pick(loop, cpu, event->sequence);
    case PACHA_SCHED_EVENT_THREAD_YIELD:
    case PACHA_SCHED_EVENT_TICK:
        loop->idle_cpu_mask |= 1ull << cpu;
        if (event->thread_id >= 4) debug_mark("[schedulerd] tick enter\n");
        if (event->thread_id != PACHA_SCHED_NO_THREAD) {
            if (loop->running_thread[cpu] == event->thread_id &&
                loop->running_generation[cpu] == event->generation) {
                clear_running_cpu(loop, cpu);
            }
            int status = charge_running(loop, event);
            if (event->thread_id >= 4) debug_mark("[schedulerd] tick charged\n");
            if (status != 0) {
                debug_log("charge-failed", event, cpu, event->thread_id, status);
            }
            debug_log("charge-done", event, cpu, event->thread_id, status);
            status = ensure_entity_on_cpu(loop, event, cpu);
            if (event->thread_id >= 4) debug_mark("[schedulerd] tick requeued\n");
            if (status != 0) {
                debug_log("requeue-failed", event, cpu, event->thread_id, status);
                return status;
            }
            debug_log("requeue-done", event, cpu, event->thread_id, status);
        }
        debug_log("commit-pick", event, cpu, event->thread_id, 0);
        if (event->thread_id >= 4) debug_mark("[schedulerd] tick commit\n");
        return commit_pick(loop, cpu, event->sequence);
    case PACHA_SCHED_EVENT_CPU_IDLE:
        loop->idle_cpu_mask |= 1ull << cpu;
        clear_running_cpu(loop, cpu);
        return commit_pick(loop, cpu, event->sequence);
    default:
        return -4;
    }
}

int pacha_sched_loop_run_once(pacha_sched_loop_t *loop)
{
    if (loop == 0 || loop->event_fd < 0) return -1;
    pacha_sched_event_t event;
    ssize_t nread = read(loop->event_fd, &event, sizeof(event));
    if (nread < 0) return -5;
    if (nread >= 0 && nread < (ssize_t)sizeof(event)) return -5;
    if (nread != (ssize_t)sizeof(event)) return -3;
    const int status = pacha_sched_loop_dispatch(loop, &event);
    return status;
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
            .thread_id = 2,
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
