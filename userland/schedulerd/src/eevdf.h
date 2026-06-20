#ifndef PACHA_SCHEDULERD_EEVDF_H
#define PACHA_SCHEDULERD_EEVDF_H

#include <stdint.h>
#include <stddef.h>

#define PACHA_EEVDF_MAX_ENTITIES 256u
#define PACHA_EEVDF_DEFAULT_WEIGHT 1024ull
#define PACHA_EEVDF_DEFAULT_SLICE_NS 4000000ull

typedef enum pacha_eevdf_state {
    PACHA_EEVDF_EMPTY = 0,
    PACHA_EEVDF_RUNNABLE = 1,
    PACHA_EEVDF_RUNNING = 2,
    PACHA_EEVDF_BLOCKED = 3,
    PACHA_EEVDF_EXITED = 4,
} pacha_eevdf_state_t;

typedef struct pacha_eevdf_entity {
    uint64_t thread_id;
    uint64_t generation;
    uint64_t weight;
    uint64_t slice_ns;
    uint64_t service_ns;
    int64_t vruntime;
    int64_t eligible_time;
    int64_t deadline;
    pacha_eevdf_state_t state;
} pacha_eevdf_entity_t;

typedef struct pacha_eevdf_runqueue {
    pacha_eevdf_entity_t entities[PACHA_EEVDF_MAX_ENTITIES];
    uint64_t entity_count;
    uint64_t runnable_count;
    int64_t virtual_time;
    int64_t min_vruntime;
} pacha_eevdf_runqueue_t;

void pacha_eevdf_init(pacha_eevdf_runqueue_t *rq);

pacha_eevdf_entity_t *pacha_eevdf_find(pacha_eevdf_runqueue_t *rq, uint64_t thread_id);
const pacha_eevdf_entity_t *pacha_eevdf_find_const(const pacha_eevdf_runqueue_t *rq, uint64_t thread_id);

int pacha_eevdf_add(
    pacha_eevdf_runqueue_t *rq,
    uint64_t thread_id,
    uint64_t generation,
    uint64_t weight,
    uint64_t slice_ns);
int pacha_eevdf_reset(
    pacha_eevdf_runqueue_t *rq,
    uint64_t thread_id,
    uint64_t generation,
    uint64_t weight,
    uint64_t slice_ns);
int pacha_eevdf_wake(pacha_eevdf_runqueue_t *rq, uint64_t thread_id);
int pacha_eevdf_block(pacha_eevdf_runqueue_t *rq, uint64_t thread_id);
int pacha_eevdf_exit(pacha_eevdf_runqueue_t *rq, uint64_t thread_id);
int pacha_eevdf_charge(pacha_eevdf_runqueue_t *rq, uint64_t thread_id, uint64_t runtime_ns);
int pacha_eevdf_import_runnable(pacha_eevdf_runqueue_t *rq, const pacha_eevdf_entity_t *source);

pacha_eevdf_entity_t *pacha_eevdf_pick(pacha_eevdf_runqueue_t *rq);
int pacha_eevdf_mark_running(pacha_eevdf_runqueue_t *rq, uint64_t thread_id);
int pacha_eevdf_requeue_running(pacha_eevdf_runqueue_t *rq, uint64_t thread_id);

int pacha_eevdf_self_check(void);

#endif
