#include "eevdf.h"

#include <limits.h>
#include <string.h>

static uint64_t clamp_weight(uint64_t weight)
{
    return weight == 0 ? PACHA_EEVDF_DEFAULT_WEIGHT : weight;
}

static uint64_t clamp_slice(uint64_t slice_ns)
{
    return slice_ns == 0 ? PACHA_EEVDF_DEFAULT_SLICE_NS : slice_ns;
}

static int64_t max_i64(int64_t lhs, int64_t rhs)
{
    return lhs > rhs ? lhs : rhs;
}

static int64_t weighted_delta(uint64_t runtime_ns, uint64_t weight)
{
    if (weight == 0) return 0;
    if (runtime_ns > (uint64_t)INT64_MAX / PACHA_EEVDF_DEFAULT_WEIGHT) {
        return INT64_MAX;
    }
    const uint64_t scaled = runtime_ns * PACHA_EEVDF_DEFAULT_WEIGHT / weight;
    return scaled > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)scaled;
}

static int64_t weighted_slice(uint64_t slice_ns, uint64_t weight)
{
    int64_t delta = weighted_delta(slice_ns, weight);
    return delta <= 0 ? 1 : delta;
}

static uint64_t entity_limit(pacha_eevdf_runqueue_t *rq)
{
    if (rq == 0) return 0;
    if (rq->entity_count > PACHA_EEVDF_MAX_ENTITIES) {
        rq->entity_count = PACHA_EEVDF_MAX_ENTITIES;
    }
    return rq->entity_count;
}

static void refresh_deadline(pacha_eevdf_entity_t *entity, int64_t floor_vruntime)
{
    entity->eligible_time = max_i64(entity->vruntime, floor_vruntime);
    entity->deadline = entity->eligible_time + weighted_slice(entity->slice_ns, entity->weight);
}

static void refresh_min_vruntime(pacha_eevdf_runqueue_t *rq)
{
    int found = 0;
    int64_t min_vruntime = rq->min_vruntime;
    uint64_t runnable_count = 0;

    const uint64_t count = entity_limit(rq);
    for (uint64_t i = 0; i < count; i++) {
        const pacha_eevdf_entity_t *entity = &rq->entities[i];
        if (entity->state != PACHA_EEVDF_RUNNABLE && entity->state != PACHA_EEVDF_RUNNING) {
            continue;
        }
        if (!found || entity->vruntime < min_vruntime) {
            min_vruntime = entity->vruntime;
            found = 1;
        }
        if (entity->state == PACHA_EEVDF_RUNNABLE) {
            runnable_count++;
        }
    }

    rq->runnable_count = runnable_count;
    if (found) {
        rq->min_vruntime = min_vruntime;
        if (rq->virtual_time < min_vruntime) {
            rq->virtual_time = min_vruntime;
        }
    }
}

static void init_entity(
    pacha_eevdf_runqueue_t *rq,
    pacha_eevdf_entity_t *entity,
    uint64_t thread_id,
    uint64_t generation,
    uint64_t weight,
    uint64_t slice_ns)
{
    memset(entity, 0, sizeof(*entity));
    entity->thread_id = thread_id;
    entity->generation = generation;
    entity->weight = clamp_weight(weight);
    entity->slice_ns = clamp_slice(slice_ns);
    entity->vruntime = rq->min_vruntime;
    entity->state = PACHA_EEVDF_RUNNABLE;
    refresh_deadline(entity, rq->min_vruntime);
}

void pacha_eevdf_init(pacha_eevdf_runqueue_t *rq)
{
    if (rq == 0) return;
    memset(rq, 0, sizeof(*rq));
}

pacha_eevdf_entity_t *pacha_eevdf_find(pacha_eevdf_runqueue_t *rq, uint64_t thread_id)
{
    if (rq == 0 || thread_id == 0) return 0;
    const uint64_t count = entity_limit(rq);
    for (uint64_t i = 0; i < count; i++) {
        if (rq->entities[i].thread_id == thread_id && rq->entities[i].state != PACHA_EEVDF_EMPTY) {
            return &rq->entities[i];
        }
    }
    return 0;
}

const pacha_eevdf_entity_t *pacha_eevdf_find_const(const pacha_eevdf_runqueue_t *rq, uint64_t thread_id)
{
    return pacha_eevdf_find((pacha_eevdf_runqueue_t *)(uintptr_t)rq, thread_id);
}

int pacha_eevdf_add(
    pacha_eevdf_runqueue_t *rq,
    uint64_t thread_id,
    uint64_t generation,
    uint64_t weight,
    uint64_t slice_ns)
{
    if (rq == 0 || thread_id == 0 || pacha_eevdf_find(rq, thread_id) != 0) {
        return -1;
    }
    if (rq->entity_count >= PACHA_EEVDF_MAX_ENTITIES) {
        return -2;
    }

    pacha_eevdf_entity_t *entity = &rq->entities[rq->entity_count++];
    init_entity(rq, entity, thread_id, generation, weight, slice_ns);
    refresh_min_vruntime(rq);
    return 0;
}

int pacha_eevdf_reset(
    pacha_eevdf_runqueue_t *rq,
    uint64_t thread_id,
    uint64_t generation,
    uint64_t weight,
    uint64_t slice_ns)
{
    if (rq == 0 || thread_id == 0) {
        return -1;
    }
    pacha_eevdf_entity_t *entity = pacha_eevdf_find(rq, thread_id);
    if (entity == 0) {
        return pacha_eevdf_add(rq, thread_id, generation, weight, slice_ns);
    }
    init_entity(rq, entity, thread_id, generation, weight, slice_ns);
    refresh_min_vruntime(rq);
    return 0;
}

int pacha_eevdf_wake(pacha_eevdf_runqueue_t *rq, uint64_t thread_id)
{
    pacha_eevdf_entity_t *entity = pacha_eevdf_find(rq, thread_id);
    if (entity == 0 || entity->state == PACHA_EEVDF_EXITED) {
        return -1;
    }
    if (entity->state == PACHA_EEVDF_RUNNABLE) {
        return 0;
    }
    entity->state = PACHA_EEVDF_RUNNABLE;
    entity->vruntime = max_i64(entity->vruntime, rq->min_vruntime);
    refresh_deadline(entity, rq->min_vruntime);
    refresh_min_vruntime(rq);
    return 0;
}

int pacha_eevdf_block(pacha_eevdf_runqueue_t *rq, uint64_t thread_id)
{
    pacha_eevdf_entity_t *entity = pacha_eevdf_find(rq, thread_id);
    if (entity == 0 || entity->state == PACHA_EEVDF_EXITED) {
        return -1;
    }
    entity->state = PACHA_EEVDF_BLOCKED;
    refresh_min_vruntime(rq);
    return 0;
}

int pacha_eevdf_exit(pacha_eevdf_runqueue_t *rq, uint64_t thread_id)
{
    pacha_eevdf_entity_t *entity = pacha_eevdf_find(rq, thread_id);
    if (entity == 0) {
        return -1;
    }
    entity->state = PACHA_EEVDF_EXITED;
    refresh_min_vruntime(rq);
    return 0;
}

int pacha_eevdf_charge(pacha_eevdf_runqueue_t *rq, uint64_t thread_id, uint64_t runtime_ns)
{
    pacha_eevdf_entity_t *entity = pacha_eevdf_find(rq, thread_id);
    if (entity == 0 ||
        (entity->state != PACHA_EEVDF_RUNNING && entity->state != PACHA_EEVDF_RUNNABLE)) {
        return -1;
    }
    const int64_t delta = weighted_delta(runtime_ns, entity->weight);
    if (delta < 0 || entity->vruntime > INT64_MAX - delta) {
        return -2;
    }
    entity->vruntime += delta;
    entity->service_ns += runtime_ns;
    refresh_deadline(entity, rq->min_vruntime);
    refresh_min_vruntime(rq);
    return 0;
}

int pacha_eevdf_import_runnable(pacha_eevdf_runqueue_t *rq, const pacha_eevdf_entity_t *source)
{
    if (rq == 0 || source == 0 || source->thread_id == 0) {
        return -1;
    }

    pacha_eevdf_entity_t *entity = pacha_eevdf_find(rq, source->thread_id);
    if (entity == 0) {
        if (rq->entity_count >= PACHA_EEVDF_MAX_ENTITIES) {
            return -2;
        }
        entity = &rq->entities[rq->entity_count++];
    }

    *entity = *source;
    entity->state = PACHA_EEVDF_RUNNABLE;
    entity->vruntime = max_i64(entity->vruntime, rq->min_vruntime);
    refresh_deadline(entity, rq->min_vruntime);
    refresh_min_vruntime(rq);
    return 0;
}

pacha_eevdf_entity_t *pacha_eevdf_pick(pacha_eevdf_runqueue_t *rq)
{
    if (rq == 0) return 0;

    pacha_eevdf_entity_t *best = 0;
    int64_t next_eligible = 0;
    int saw_future = 0;

    const uint64_t count = entity_limit(rq);
    for (uint64_t i = 0; i < count; i++) {
        pacha_eevdf_entity_t *entity = &rq->entities[i];
        if (entity->state != PACHA_EEVDF_RUNNABLE) {
            continue;
        }
        if (entity->eligible_time > rq->virtual_time) {
            if (!saw_future || entity->eligible_time < next_eligible) {
                next_eligible = entity->eligible_time;
                saw_future = 1;
            }
            continue;
        }
        if (best == 0 ||
            entity->deadline < best->deadline ||
            (entity->deadline == best->deadline && entity->thread_id < best->thread_id)) {
            best = entity;
        }
    }

    if (best != 0) {
        return best;
    }
    if (!saw_future) {
        return 0;
    }

    rq->virtual_time = next_eligible;
    for (uint64_t i = 0; i < count; i++) {
        pacha_eevdf_entity_t *entity = &rq->entities[i];
        if (entity->state != PACHA_EEVDF_RUNNABLE || entity->eligible_time > rq->virtual_time) {
            continue;
        }
        if (best == 0 ||
            entity->deadline < best->deadline ||
            (entity->deadline == best->deadline && entity->thread_id < best->thread_id)) {
            best = entity;
        }
    }
    return best;
}

int pacha_eevdf_mark_running(pacha_eevdf_runqueue_t *rq, uint64_t thread_id)
{
    pacha_eevdf_entity_t *entity = pacha_eevdf_find(rq, thread_id);
    if (entity == 0 || entity->state != PACHA_EEVDF_RUNNABLE) {
        return -1;
    }
    entity->state = PACHA_EEVDF_RUNNING;
    refresh_min_vruntime(rq);
    return 0;
}

int pacha_eevdf_requeue_running(pacha_eevdf_runqueue_t *rq, uint64_t thread_id)
{
    pacha_eevdf_entity_t *entity = pacha_eevdf_find(rq, thread_id);
    if (entity == 0 || entity->state != PACHA_EEVDF_RUNNING) {
        return -1;
    }
    entity->state = PACHA_EEVDF_RUNNABLE;
    refresh_deadline(entity, rq->min_vruntime);
    refresh_min_vruntime(rq);
    return 0;
}

int pacha_eevdf_self_check(void)
{
    static pacha_eevdf_runqueue_t rq;
    pacha_eevdf_init(&rq);

    if (pacha_eevdf_add(&rq, 10, 1, 1024, 4000000) != 0) return -1;
    if (pacha_eevdf_add(&rq, 20, 1, 1024, 1000000) != 0) return -2;
    if (pacha_eevdf_add(&rq, 30, 1, 256, 4000000) != 0) return -3;

    pacha_eevdf_entity_t *picked = pacha_eevdf_pick(&rq);
    if (picked == 0 || picked->thread_id != 20) return -4;
    if (pacha_eevdf_mark_running(&rq, picked->thread_id) != 0) return -5;
    if (pacha_eevdf_charge(&rq, picked->thread_id, 1000000) != 0) return -6;
    if (pacha_eevdf_requeue_running(&rq, picked->thread_id) != 0) return -7;

    if (pacha_eevdf_block(&rq, 20) != 0) return -8;
    picked = pacha_eevdf_pick(&rq);
    if (picked == 0 || picked->thread_id != 10) return -9;

    if (pacha_eevdf_exit(&rq, 10) != 0) return -10;
    if (pacha_eevdf_wake(&rq, 20) != 0) return -11;
    picked = pacha_eevdf_pick(&rq);
    if (picked == 0 || picked->thread_id != 30) return -12;
    if (pacha_eevdf_block(&rq, 30) != 0) return -13;
    picked = pacha_eevdf_pick(&rq);
    if (picked == 0 || picked->thread_id != 20) return -14;

    if (pacha_eevdf_exit(&rq, 20) != 0) return -15;
    if (pacha_eevdf_reset(&rq, 20, 2, 1024, 1000000) != 0) return -16;
    picked = pacha_eevdf_pick(&rq);
    if (picked == 0 || picked->thread_id != 20 || picked->generation != 2) return -17;

    return 0;
}
