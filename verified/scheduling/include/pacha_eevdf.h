#ifndef PACHA_EEVDF_H
#define PACHA_EEVDF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PACHA_EEVDF_MAX_ENTITIES 256u
#define PACHA_EEVDF_DEFAULT_WEIGHT 1024
#define PACHA_EEVDF_DEFAULT_SLICE_NS 4000000
#define PACHA_EEVDF_NO_THREAD_ID 0

typedef enum pacha_eevdf_state {
  PACHA_EEVDF_EMPTY = 0,
  PACHA_EEVDF_RUNNABLE = 1,
  PACHA_EEVDF_RUNNING = 2,
  PACHA_EEVDF_BLOCKED = 3,
  PACHA_EEVDF_EXITED = 4,
} pacha_eevdf_state;

typedef enum pacha_eevdf_rc {
  PACHA_EEVDF_OK = 0,
  PACHA_EEVDF_ERR_INVALID = 1,
  PACHA_EEVDF_ERR_FULL = 2,
  PACHA_EEVDF_ERR_OVERFLOW = 3,
  PACHA_EEVDF_ERR_STATE = 4,
} pacha_eevdf_rc;

typedef struct pacha_eevdf_entity {
  int64_t thread_id;
  int64_t generation;
  int64_t weight;
  int64_t slice_ns;
  int64_t service_ns;
  int64_t vruntime;
  int64_t eligible_time;
  int64_t deadline;
  pacha_eevdf_state state;
} pacha_eevdf_entity;

typedef struct pacha_eevdf_runqueue {
  pacha_eevdf_entity entities[PACHA_EEVDF_MAX_ENTITIES];
  size_t entity_count;
  size_t runnable_count;
  int64_t virtual_time;
  int64_t min_vruntime;
} pacha_eevdf_runqueue;

typedef struct pacha_eevdf_pick_result {
  pacha_eevdf_runqueue rq;
  int has_entity;
  size_t index;
  pacha_eevdf_entity entity;
} pacha_eevdf_pick_result;

void pacha_eevdf_empty_entity(pacha_eevdf_entity *out);
void pacha_eevdf_empty_runqueue(pacha_eevdf_runqueue *out);
void pacha_eevdf_copy_runqueue(
    const pacha_eevdf_runqueue *src,
    pacha_eevdf_runqueue *dst);

pacha_eevdf_rc pacha_eevdf_reset(
    const pacha_eevdf_runqueue *rq,
    pacha_eevdf_runqueue *out);
pacha_eevdf_rc pacha_eevdf_add(
    const pacha_eevdf_runqueue *rq,
    int64_t thread_id,
    int64_t generation,
    int64_t weight,
    int64_t slice_ns,
    pacha_eevdf_runqueue *out);
pacha_eevdf_rc pacha_eevdf_wake(
    const pacha_eevdf_runqueue *rq,
    int64_t thread_id,
    pacha_eevdf_runqueue *out);
pacha_eevdf_rc pacha_eevdf_block(
    const pacha_eevdf_runqueue *rq,
    int64_t thread_id,
    pacha_eevdf_runqueue *out);
pacha_eevdf_rc pacha_eevdf_exit(
    const pacha_eevdf_runqueue *rq,
    int64_t thread_id,
    pacha_eevdf_runqueue *out);
pacha_eevdf_rc pacha_eevdf_charge(
    const pacha_eevdf_runqueue *rq,
    int64_t thread_id,
    int64_t runtime_ns,
    pacha_eevdf_runqueue *out);
pacha_eevdf_rc pacha_eevdf_mark_running(
    const pacha_eevdf_runqueue *rq,
    int64_t thread_id,
    pacha_eevdf_runqueue *out);
pacha_eevdf_rc pacha_eevdf_requeue_running(
    const pacha_eevdf_runqueue *rq,
    int64_t thread_id,
    pacha_eevdf_runqueue *out);
pacha_eevdf_rc pacha_eevdf_pick(
    const pacha_eevdf_runqueue *rq,
    pacha_eevdf_pick_result *out);

#ifdef __cplusplus
}
#endif

#endif
