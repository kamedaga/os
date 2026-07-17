#include "pacha_eevdf.h"

#include <limits.h>

#if PACHA_EEVDF_DEFAULT_WEIGHT != 1024
#error "weighted_delta expects PACHA_EEVDF_DEFAULT_WEIGHT to be 1024"
#endif

static int valid_positive(int64_t value) {
  return value > 0;
}

static int i64_nonnegative(int64_t value) {
  return value >= 0;
}

static int i64_less(int64_t lhs, int64_t rhs) {
  return lhs < rhs;
}

static int i64_equal(int64_t lhs, int64_t rhs) {
  return lhs == rhs;
}

static int checked_add_i64_overflows(int64_t lhs, int64_t rhs) {
  if (i64_less(0, rhs)) {
    return i64_less(INT64_MAX - rhs, lhs);
  }
  if (i64_less(rhs, 0)) {
    return i64_less(lhs, INT64_MIN - rhs);
  }
  return 0;
}

static int checked_add_i64(int64_t lhs, int64_t rhs, int64_t *out) {
  if (checked_add_i64_overflows(lhs, rhs)) {
    return 0;
  }
  *out = lhs + rhs;
  return 1;
}

static int checked_mul_i64_nonnegative_overflows(int64_t lhs, int64_t rhs) {
  if (rhs == 0) {
    return 0;
  }
  return lhs > INT64_MAX / rhs;
}

static int checked_mul_i64_nonnegative(
    int64_t lhs,
    int64_t rhs,
    int64_t *out) {
  if (!i64_nonnegative(lhs)) {
    return 0;
  }
  if (!i64_nonnegative(rhs)) {
    return 0;
  }
  if (checked_mul_i64_nonnegative_overflows(lhs, rhs)) {
    return 0;
  }
  *out = lhs * rhs;
  return 1;
}

static int64_t z_max(int64_t lhs, int64_t rhs) {
  if (i64_less(lhs, rhs)) {
    return rhs;
  }
  return lhs;
}

static int is_runnable(const pacha_eevdf_entity *entity) {
  return entity->state == PACHA_EEVDF_RUNNABLE;
}

static int live_for_min(const pacha_eevdf_entity *entity) {
  return entity->state == PACHA_EEVDF_RUNNABLE ||
         entity->state == PACHA_EEVDF_RUNNING;
}

static int is_active_state(pacha_eevdf_state state) {
  return state != PACHA_EEVDF_EMPTY;
}

static int runnable_or_running(const pacha_eevdf_entity *entity) {
  return entity->state == PACHA_EEVDF_RUNNABLE ||
         entity->state == PACHA_EEVDF_RUNNING;
}

static pacha_eevdf_rc ok_runqueue(
    const pacha_eevdf_runqueue *rq,
    pacha_eevdf_runqueue *out) {
  *out = *rq;
  return PACHA_EEVDF_OK;
}

static pacha_eevdf_rc fail_runqueue(
    pacha_eevdf_rc rc,
    const pacha_eevdf_runqueue *rq,
    pacha_eevdf_runqueue *out) {
  *out = *rq;
  return rc;
}

void pacha_eevdf_copy_runqueue(
    const pacha_eevdf_runqueue *src,
    pacha_eevdf_runqueue *dst) {
  for (size_t i = 0; i < PACHA_EEVDF_MAX_ENTITIES; ++i) {
    dst->entities[i].thread_id = src->entities[i].thread_id;
    dst->entities[i].generation = src->entities[i].generation;
    dst->entities[i].weight = src->entities[i].weight;
    dst->entities[i].slice_ns = src->entities[i].slice_ns;
    dst->entities[i].service_ns = src->entities[i].service_ns;
    dst->entities[i].vruntime = src->entities[i].vruntime;
    dst->entities[i].eligible_time = src->entities[i].eligible_time;
    dst->entities[i].deadline = src->entities[i].deadline;
    dst->entities[i].state = src->entities[i].state;
  }
  dst->entity_count = src->entity_count;
  dst->runnable_count = src->runnable_count;
  dst->virtual_time = src->virtual_time;
  dst->min_vruntime = src->min_vruntime;
  return;
}

static size_t count_runnable(const pacha_eevdf_runqueue *rq) {
  size_t count = 0;
  for (size_t i = 0; i < rq->entity_count; ++i) {
    if (is_runnable(&rq->entities[i])) {
      ++count;
    }
  }
  return count;
}

static int weighted_fractional_10(
    int64_t remainder,
    int64_t weight,
    int64_t *out) {
  int64_t fractional = 0;
  for (int bit = 0; bit < 10; ++bit) {
    fractional = fractional * 2;
    if (remainder >= weight - remainder) {
      fractional += 1;
      remainder = remainder - (weight - remainder);
    } else {
      remainder = remainder + remainder;
    }
  }
  *out = fractional;
  return 1;
}

static int weighted_delta(int64_t runtime_ns, int64_t weight, int64_t *out) {
  if (!i64_nonnegative(runtime_ns)) {
    return 0;
  }
  if (!valid_positive(weight)) {
    return 0;
  }
  int64_t quotient = runtime_ns / weight;
  int64_t remainder = runtime_ns % weight;
  int64_t whole = 0;
  if (!checked_mul_i64_nonnegative(
          quotient,
          PACHA_EEVDF_DEFAULT_WEIGHT,
          &whole)) {
    return 0;
  }

  int64_t fractional = 0;
  if (!weighted_fractional_10(remainder, weight, &fractional)) {
    return 0;
  }
  int64_t delta = 0;
  if (!checked_add_i64(whole, fractional, &delta)) {
    return 0;
  }
  *out = delta;
  return 1;
}

static int weighted_slice(int64_t slice_ns, int64_t weight, int64_t *out) {
  int64_t delta;
  if (!weighted_delta(slice_ns, weight, &delta)) {
    return 0;
  }
  *out = z_max(1, delta);
  return 1;
}

static int refresh_deadline(
    const pacha_eevdf_entity *entity,
    int64_t floor_vruntime,
    pacha_eevdf_entity *out) {
  int64_t slice;
  int64_t deadline;
  if (!weighted_slice(entity->slice_ns, entity->weight, &slice)) {
    return 0;
  }
  int64_t eligible = z_max(entity->vruntime, floor_vruntime);
  if (!checked_add_i64(eligible, slice, &deadline)) {
    return 0;
  }
  *out = *entity;
  out->eligible_time = eligible;
  out->deadline = deadline;
  return 1;
}

static void refresh_runqueue(pacha_eevdf_runqueue *rq) {
  int found = 0;
  int64_t min_vruntime = rq->min_vruntime;
  for (size_t i = 0; i < rq->entity_count; ++i) {
    if (!live_for_min(&rq->entities[i])) {
      continue;
    }
    if (!found || rq->entities[i].vruntime < min_vruntime) {
      min_vruntime = rq->entities[i].vruntime;
    }
    found = 1;
  }
  rq->runnable_count = count_runnable(rq);
  rq->virtual_time = z_max(rq->virtual_time, min_vruntime);
  rq->min_vruntime = min_vruntime;
}

static void recount_runqueue(pacha_eevdf_runqueue *rq) {
  rq->runnable_count = count_runnable(rq);
}

static void set_entity_state(
    pacha_eevdf_entity *entity,
    pacha_eevdf_state state) {
  entity->state = state;
}

static void remove_entity_at(
    const pacha_eevdf_runqueue *rq,
    size_t index,
    pacha_eevdf_runqueue *out) {
  *out = *rq;
  for (size_t i = index; i + 1 < rq->entity_count; ++i) {
    out->entities[i] = rq->entities[i + 1];
  }
  --out->entity_count;
  pacha_eevdf_empty_entity(&out->entities[out->entity_count]);
}

static void place_entity_at_floor(
    pacha_eevdf_entity *entity,
    int64_t floor_vruntime) {
  entity->vruntime = z_max(entity->vruntime, floor_vruntime);
}

static int find_entity_index(
    const pacha_eevdf_runqueue *rq,
    int64_t thread_id,
    size_t *index_out) {
  if (thread_id == PACHA_EEVDF_NO_THREAD_ID) {
    return 0;
  }
  for (size_t i = 0; i < rq->entity_count; ++i) {
    if (rq->entities[i].thread_id == thread_id &&
        is_active_state(rq->entities[i].state)) {
      *index_out = i;
      return 1;
    }
  }
  return 0;
}

static int entity_better_values(
    int64_t candidate_deadline,
    int64_t current_deadline,
    int64_t candidate_thread_id,
    int64_t current_thread_id) {
  int result = 0;
  if (i64_less(candidate_deadline, current_deadline)) {
    result = 1;
  } else if (i64_equal(candidate_deadline, current_deadline)) {
    result = i64_less(candidate_thread_id, current_thread_id);
  }
  return result;
}

static int entity_better(
    const pacha_eevdf_entity *candidate,
    const pacha_eevdf_entity *current) {
  int result = entity_better_values(
      candidate->deadline,
      current->deadline,
      candidate->thread_id,
      current->thread_id);
  return result;
}

void pacha_eevdf_empty_entity(pacha_eevdf_entity *out) {
  out->thread_id = PACHA_EEVDF_NO_THREAD_ID;
  out->generation = 0;
  out->weight = 0;
  out->slice_ns = 0;
  out->service_ns = 0;
  out->vruntime = 0;
  out->eligible_time = 0;
  out->deadline = 0;
  out->state = PACHA_EEVDF_EMPTY;
  return;
}

static pacha_eevdf_rc fail_entity(
    pacha_eevdf_rc rc,
    const pacha_eevdf_entity *entity,
    pacha_eevdf_entity *out) {
  *out = *entity;
  return rc;
}

pacha_eevdf_rc pacha_eevdf_entity_validate(
    const pacha_eevdf_entity *entity) {
  int64_t slice;
  int64_t expected_deadline;
  if (entity->state == PACHA_EEVDF_EMPTY) {
    return entity->thread_id == PACHA_EEVDF_NO_THREAD_ID &&
               entity->generation == 0 &&
               entity->weight == 0 &&
               entity->slice_ns == 0 &&
               entity->service_ns == 0 &&
               entity->vruntime == 0 &&
               entity->eligible_time == 0 &&
               entity->deadline == 0
        ? PACHA_EEVDF_OK
        : PACHA_EEVDF_ERR_STATE;
  }
  if (entity->state != PACHA_EEVDF_RUNNABLE &&
      entity->state != PACHA_EEVDF_RUNNING &&
      entity->state != PACHA_EEVDF_BLOCKED &&
      entity->state != PACHA_EEVDF_EXITED) {
    return PACHA_EEVDF_ERR_STATE;
  }
  if (entity->thread_id == PACHA_EEVDF_NO_THREAD_ID ||
      !valid_positive(entity->generation) ||
      !valid_positive(entity->weight) ||
      !valid_positive(entity->slice_ns)) {
    return PACHA_EEVDF_ERR_INVALID;
  }
  if (!i64_nonnegative(entity->service_ns) ||
      !i64_nonnegative(entity->vruntime) ||
      entity->eligible_time < entity->vruntime) {
    return PACHA_EEVDF_ERR_STATE;
  }
  if (!weighted_slice(entity->slice_ns, entity->weight, &slice) ||
      !checked_add_i64(entity->eligible_time, slice, &expected_deadline)) {
    return PACHA_EEVDF_ERR_OVERFLOW;
  }
  if (entity->deadline != expected_deadline) {
    return PACHA_EEVDF_ERR_STATE;
  }
  return PACHA_EEVDF_OK;
}

pacha_eevdf_rc pacha_eevdf_entity_init(
    int64_t thread_id,
    int64_t generation,
    int64_t weight,
    int64_t slice_ns,
    int64_t floor_vruntime,
    pacha_eevdf_entity *out) {
  pacha_eevdf_entity entity;
  pacha_eevdf_empty_entity(out);
  if (thread_id == PACHA_EEVDF_NO_THREAD_ID ||
      !valid_positive(generation) ||
      !valid_positive(weight) ||
      !valid_positive(slice_ns) ||
      !i64_nonnegative(floor_vruntime)) {
    return PACHA_EEVDF_ERR_INVALID;
  }
  pacha_eevdf_empty_entity(&entity);
  entity.thread_id = thread_id;
  entity.generation = generation;
  entity.weight = weight;
  entity.slice_ns = slice_ns;
  entity.vruntime = floor_vruntime;
  entity.state = PACHA_EEVDF_RUNNABLE;
  if (!refresh_deadline(&entity, floor_vruntime, out)) {
    pacha_eevdf_empty_entity(out);
    return PACHA_EEVDF_ERR_OVERFLOW;
  }
  return PACHA_EEVDF_OK;
}

pacha_eevdf_rc pacha_eevdf_entity_wake(
    const pacha_eevdf_entity *entity,
    int64_t floor_vruntime,
    pacha_eevdf_entity *out) {
  pacha_eevdf_entity next;
  pacha_eevdf_rc validation = pacha_eevdf_entity_validate(entity);
  if (validation != PACHA_EEVDF_OK) {
    return fail_entity(validation, entity, out);
  }
  if (entity->state != PACHA_EEVDF_BLOCKED) {
    return fail_entity(PACHA_EEVDF_ERR_STATE, entity, out);
  }
  if (!i64_nonnegative(floor_vruntime)) {
    return fail_entity(PACHA_EEVDF_ERR_INVALID, entity, out);
  }
  next = *entity;
  place_entity_at_floor(&next, floor_vruntime);
  next.state = PACHA_EEVDF_RUNNABLE;
  if (!refresh_deadline(&next, floor_vruntime, out)) {
    return fail_entity(PACHA_EEVDF_ERR_OVERFLOW, entity, out);
  }
  return PACHA_EEVDF_OK;
}

pacha_eevdf_rc pacha_eevdf_entity_block(
    const pacha_eevdf_entity *entity,
    pacha_eevdf_entity *out) {
  pacha_eevdf_rc validation = pacha_eevdf_entity_validate(entity);
  if (validation != PACHA_EEVDF_OK) {
    return fail_entity(validation, entity, out);
  }
  if (!runnable_or_running(entity)) {
    return fail_entity(PACHA_EEVDF_ERR_STATE, entity, out);
  }
  *out = *entity;
  out->state = PACHA_EEVDF_BLOCKED;
  return PACHA_EEVDF_OK;
}

pacha_eevdf_rc pacha_eevdf_entity_exit(
    const pacha_eevdf_entity *entity,
    pacha_eevdf_entity *out) {
  pacha_eevdf_rc validation = pacha_eevdf_entity_validate(entity);
  if (validation != PACHA_EEVDF_OK) {
    return fail_entity(validation, entity, out);
  }
  if (entity->state == PACHA_EEVDF_EMPTY ||
      entity->state == PACHA_EEVDF_EXITED) {
    return fail_entity(PACHA_EEVDF_ERR_STATE, entity, out);
  }
  *out = *entity;
  out->state = PACHA_EEVDF_EXITED;
  return PACHA_EEVDF_OK;
}

pacha_eevdf_rc pacha_eevdf_entity_charge(
    const pacha_eevdf_entity *entity,
    int64_t runtime_ns,
    int64_t floor_vruntime,
    pacha_eevdf_entity *out) {
  int64_t delta;
  int64_t vruntime;
  int64_t service_ns;
  pacha_eevdf_entity next;
  pacha_eevdf_rc validation = pacha_eevdf_entity_validate(entity);
  if (validation != PACHA_EEVDF_OK) {
    return fail_entity(validation, entity, out);
  }
  if (!runnable_or_running(entity)) {
    return fail_entity(PACHA_EEVDF_ERR_STATE, entity, out);
  }
  if (!i64_nonnegative(runtime_ns) || !i64_nonnegative(floor_vruntime)) {
    return fail_entity(PACHA_EEVDF_ERR_INVALID, entity, out);
  }
  if (!weighted_delta(runtime_ns, entity->weight, &delta) ||
      !checked_add_i64(entity->vruntime, delta, &vruntime) ||
      !checked_add_i64(entity->service_ns, runtime_ns, &service_ns)) {
    return fail_entity(PACHA_EEVDF_ERR_OVERFLOW, entity, out);
  }
  next = *entity;
  next.service_ns = service_ns;
  next.vruntime = vruntime;
  if (!refresh_deadline(&next, floor_vruntime, out)) {
    return fail_entity(PACHA_EEVDF_ERR_OVERFLOW, entity, out);
  }
  return PACHA_EEVDF_OK;
}

pacha_eevdf_rc pacha_eevdf_entity_mark_running(
    const pacha_eevdf_entity *entity,
    pacha_eevdf_entity *out) {
  pacha_eevdf_rc validation = pacha_eevdf_entity_validate(entity);
  if (validation != PACHA_EEVDF_OK) {
    return fail_entity(validation, entity, out);
  }
  if (entity->state != PACHA_EEVDF_RUNNABLE) {
    return fail_entity(PACHA_EEVDF_ERR_STATE, entity, out);
  }
  *out = *entity;
  out->state = PACHA_EEVDF_RUNNING;
  return PACHA_EEVDF_OK;
}

pacha_eevdf_rc pacha_eevdf_entity_finish(
    const pacha_eevdf_entity *entity,
    pacha_eevdf_entity *out) {
  pacha_eevdf_rc validation = pacha_eevdf_entity_validate(entity);
  if (validation != PACHA_EEVDF_OK) {
    return fail_entity(validation, entity, out);
  }
  if (entity->state != PACHA_EEVDF_RUNNING) {
    return fail_entity(PACHA_EEVDF_ERR_STATE, entity, out);
  }
  *out = *entity;
  out->state = PACHA_EEVDF_RUNNABLE;
  return PACHA_EEVDF_OK;
}

pacha_eevdf_rc pacha_eevdf_entity_migrate(
    const pacha_eevdf_entity *entity,
    int64_t floor_vruntime,
    pacha_eevdf_entity *out) {
  pacha_eevdf_entity next;
  pacha_eevdf_rc validation = pacha_eevdf_entity_validate(entity);
  if (validation != PACHA_EEVDF_OK) {
    return fail_entity(validation, entity, out);
  }
  if (entity->state != PACHA_EEVDF_RUNNABLE) {
    return fail_entity(PACHA_EEVDF_ERR_STATE, entity, out);
  }
  if (!i64_nonnegative(floor_vruntime)) {
    return fail_entity(PACHA_EEVDF_ERR_INVALID, entity, out);
  }
  next = *entity;
  place_entity_at_floor(&next, floor_vruntime);
  if (!refresh_deadline(&next, floor_vruntime, out)) {
    return fail_entity(PACHA_EEVDF_ERR_OVERFLOW, entity, out);
  }
  return PACHA_EEVDF_OK;
}

void pacha_eevdf_empty_runqueue(pacha_eevdf_runqueue *out) {
  for (size_t i = 0; i < PACHA_EEVDF_MAX_ENTITIES; ++i) {
    pacha_eevdf_empty_entity(&out->entities[i]);
  }
  out->entity_count = 0;
  out->runnable_count = 0;
  out->virtual_time = 0;
  out->min_vruntime = 0;
  return;
}

pacha_eevdf_rc pacha_eevdf_reset(
    const pacha_eevdf_runqueue *rq,
    pacha_eevdf_runqueue *out) {
  (void)rq;
  pacha_eevdf_empty_runqueue(out);
  return PACHA_EEVDF_OK;
}

pacha_eevdf_rc pacha_eevdf_add(
    const pacha_eevdf_runqueue *rq,
    int64_t thread_id,
    int64_t generation,
    int64_t weight,
    int64_t slice_ns,
    pacha_eevdf_runqueue *out) {
  size_t existing;
  pacha_eevdf_entity entity;
  pacha_eevdf_entity refreshed;
  if (thread_id == PACHA_EEVDF_NO_THREAD_ID ||
      !valid_positive(weight) ||
      !valid_positive(slice_ns)) {
    return fail_runqueue(PACHA_EEVDF_ERR_INVALID, rq, out);
  }
  if (find_entity_index(rq, thread_id, &existing)) {
    (void)existing;
    return fail_runqueue(PACHA_EEVDF_ERR_INVALID, rq, out);
  }
  if (rq->entity_count >= PACHA_EEVDF_MAX_ENTITIES) {
    return fail_runqueue(PACHA_EEVDF_ERR_FULL, rq, out);
  }
  pacha_eevdf_empty_entity(&entity);
  entity.thread_id = thread_id;
  entity.generation = generation;
  entity.weight = weight;
  entity.slice_ns = slice_ns;
  entity.vruntime = rq->min_vruntime;
  entity.eligible_time = rq->min_vruntime;
  entity.deadline = rq->min_vruntime;
  entity.state = PACHA_EEVDF_RUNNABLE;
  if (!refresh_deadline(&entity, rq->min_vruntime, &refreshed)) {
    return fail_runqueue(PACHA_EEVDF_ERR_OVERFLOW, rq, out);
  }
  pacha_eevdf_runqueue next = *rq;
  next.entities[next.entity_count] = refreshed;
  ++next.entity_count;
  refresh_runqueue(&next);
  return ok_runqueue(&next, out);
}

pacha_eevdf_rc pacha_eevdf_wake(
    const pacha_eevdf_runqueue *rq,
    int64_t thread_id,
    pacha_eevdf_runqueue *out) {
  size_t index;
  pacha_eevdf_entity entity;
  pacha_eevdf_entity refreshed;
  if (!find_entity_index(rq, thread_id, &index)) {
    return fail_runqueue(PACHA_EEVDF_ERR_INVALID, rq, out);
  }
  entity = rq->entities[index];
  if (entity.state != PACHA_EEVDF_BLOCKED) {
    return fail_runqueue(PACHA_EEVDF_ERR_STATE, rq, out);
  }
  place_entity_at_floor(&entity, rq->min_vruntime);
  set_entity_state(&entity, PACHA_EEVDF_RUNNABLE);
  if (!refresh_deadline(&entity, rq->min_vruntime, &refreshed)) {
    return fail_runqueue(PACHA_EEVDF_ERR_OVERFLOW, rq, out);
  }
  pacha_eevdf_runqueue next = *rq;
  next.entities[index] = refreshed;
  refresh_runqueue(&next);
  return ok_runqueue(&next, out);
}

pacha_eevdf_rc pacha_eevdf_block(
    const pacha_eevdf_runqueue *rq,
    int64_t thread_id,
    pacha_eevdf_runqueue *out) {
  size_t index;
  pacha_eevdf_entity entity;
  if (!find_entity_index(rq, thread_id, &index)) {
    return fail_runqueue(PACHA_EEVDF_ERR_INVALID, rq, out);
  }
  entity = rq->entities[index];
  if (!runnable_or_running(&entity)) {
    return fail_runqueue(PACHA_EEVDF_ERR_STATE, rq, out);
  }
  pacha_eevdf_runqueue next = *rq;
  set_entity_state(&next.entities[index], PACHA_EEVDF_BLOCKED);
  refresh_runqueue(&next);
  return ok_runqueue(&next, out);
}

pacha_eevdf_rc pacha_eevdf_exit(
    const pacha_eevdf_runqueue *rq,
    int64_t thread_id,
    pacha_eevdf_runqueue *out) {
  size_t index;
  pacha_eevdf_entity entity;
  if (!find_entity_index(rq, thread_id, &index)) {
    return fail_runqueue(PACHA_EEVDF_ERR_INVALID, rq, out);
  }
  entity = rq->entities[index];
  if (entity.state == PACHA_EEVDF_EMPTY ||
      entity.state == PACHA_EEVDF_EXITED) {
    return fail_runqueue(PACHA_EEVDF_ERR_STATE, rq, out);
  }
  pacha_eevdf_runqueue next = *rq;
  remove_entity_at(rq, index, &next);
  refresh_runqueue(&next);
  return ok_runqueue(&next, out);
}

pacha_eevdf_rc pacha_eevdf_charge(
    const pacha_eevdf_runqueue *rq,
    int64_t thread_id,
    int64_t runtime_ns,
    pacha_eevdf_runqueue *out) {
  size_t index;
  int64_t delta;
  int64_t vruntime;
  int64_t service_ns;
  pacha_eevdf_entity entity;
  pacha_eevdf_entity refreshed;
  if (!find_entity_index(rq, thread_id, &index)) {
    return fail_runqueue(PACHA_EEVDF_ERR_INVALID, rq, out);
  }
  entity = rq->entities[index];
  if (!runnable_or_running(&entity)) {
    return fail_runqueue(PACHA_EEVDF_ERR_STATE, rq, out);
  }
  if (!weighted_delta(runtime_ns, entity.weight, &delta)) {
    return fail_runqueue(PACHA_EEVDF_ERR_OVERFLOW, rq, out);
  }
  if (!checked_add_i64(entity.vruntime, delta, &vruntime)) {
    return fail_runqueue(PACHA_EEVDF_ERR_OVERFLOW, rq, out);
  }
  if (!checked_add_i64(entity.service_ns, runtime_ns, &service_ns)) {
    return fail_runqueue(PACHA_EEVDF_ERR_OVERFLOW, rq, out);
  }
  entity.service_ns = service_ns;
  entity.vruntime = vruntime;
  if (!refresh_deadline(&entity, rq->min_vruntime, &refreshed)) {
    return fail_runqueue(PACHA_EEVDF_ERR_OVERFLOW, rq, out);
  }
  pacha_eevdf_runqueue next = *rq;
  next.entities[index] = refreshed;
  refresh_runqueue(&next);
  return ok_runqueue(&next, out);
}

pacha_eevdf_rc pacha_eevdf_mark_running(
    const pacha_eevdf_runqueue *rq,
    int64_t thread_id,
    pacha_eevdf_runqueue *out) {
  size_t index;
  pacha_eevdf_entity entity;
  if (!find_entity_index(rq, thread_id, &index)) {
    return fail_runqueue(PACHA_EEVDF_ERR_INVALID, rq, out);
  }
  entity = rq->entities[index];
  if (entity.state != PACHA_EEVDF_RUNNABLE) {
    return fail_runqueue(PACHA_EEVDF_ERR_STATE, rq, out);
  }
  pacha_eevdf_runqueue next = *rq;
  set_entity_state(&next.entities[index], PACHA_EEVDF_RUNNING);
  recount_runqueue(&next);
  return ok_runqueue(&next, out);
}

pacha_eevdf_rc pacha_eevdf_requeue_running(
    const pacha_eevdf_runqueue *rq,
    int64_t thread_id,
    pacha_eevdf_runqueue *out) {
  size_t index;
  pacha_eevdf_entity entity;
  pacha_eevdf_entity refreshed;
  if (!find_entity_index(rq, thread_id, &index)) {
    return fail_runqueue(PACHA_EEVDF_ERR_INVALID, rq, out);
  }
  entity = rq->entities[index];
  if (entity.state != PACHA_EEVDF_RUNNING) {
    return fail_runqueue(PACHA_EEVDF_ERR_STATE, rq, out);
  }
  set_entity_state(&entity, PACHA_EEVDF_RUNNABLE);
  if (!refresh_deadline(&entity, rq->min_vruntime, &refreshed)) {
    return fail_runqueue(PACHA_EEVDF_ERR_OVERFLOW, rq, out);
  }
  pacha_eevdf_runqueue next = *rq;
  next.entities[index] = refreshed;
  refresh_runqueue(&next);
  return ok_runqueue(&next, out);
}

pacha_eevdf_rc pacha_eevdf_pick(
    const pacha_eevdf_runqueue *rq,
    pacha_eevdf_pick_result *out) {
  out->rq = *rq;
  out->has_entity = 0;
  out->index = 0;
  pacha_eevdf_empty_entity(&out->entity);

  size_t best_index = 0;
  int have_best = 0;
  for (size_t i = 0; i < rq->entity_count; ++i) {
    const pacha_eevdf_entity *entity = &rq->entities[i];
    if (!is_runnable(entity) || entity->eligible_time > rq->virtual_time) {
      continue;
    }
    if (!have_best || entity_better(entity, &out->entity)) {
      out->entity = *entity;
      best_index = i;
      have_best = 1;
    }
  }
  if (have_best) {
    out->has_entity = 1;
    out->index = best_index;
    return PACHA_EEVDF_OK;
  }

  int have_next = 0;
  int64_t next_time = 0;
  for (size_t i = 0; i < rq->entity_count; ++i) {
    const pacha_eevdf_entity *entity = &rq->entities[i];
    if (!is_runnable(entity)) {
      continue;
    }
    if (!have_next || entity->eligible_time < next_time) {
      next_time = entity->eligible_time;
      have_next = 1;
    }
  }
  if (!have_next) {
    return PACHA_EEVDF_OK;
  }

  out->rq.virtual_time = next_time;
  for (size_t i = 0; i < out->rq.entity_count; ++i) {
    const pacha_eevdf_entity *entity = &out->rq.entities[i];
    if (!is_runnable(entity) || entity->eligible_time > next_time) {
      continue;
    }
    if (!out->has_entity || entity_better(entity, &out->entity)) {
      out->entity = *entity;
      out->index = i;
      out->has_entity = 1;
    }
  }
  return PACHA_EEVDF_OK;
}
