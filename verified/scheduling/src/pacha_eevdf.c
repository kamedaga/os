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

static int checked_add_i64(int64_t lhs, int64_t rhs, int64_t *out) {
  if ((rhs > 0 && lhs > INT64_MAX - rhs) ||
      (rhs < 0 && lhs < INT64_MIN - rhs)) {
    return 0;
  }
  *out = lhs + rhs;
  return 1;
}

static int checked_mul_i64_nonnegative(
    int64_t lhs,
    int64_t rhs,
    int64_t *out) {
  if (lhs < 0 || rhs < 0) {
    return 0;
  }
  if (rhs != 0 && lhs > INT64_MAX / rhs) {
    return 0;
  }
  *out = lhs * rhs;
  return 1;
}

static int64_t z_max(int64_t lhs, int64_t rhs) {
  return lhs < rhs ? rhs : lhs;
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

static pacha_eevdf_result ok_result(pacha_eevdf_runqueue rq) {
  pacha_eevdf_result result;
  result.rc = PACHA_EEVDF_OK;
  result.rq = rq;
  return result;
}

static pacha_eevdf_result fail_result(
    pacha_eevdf_rc rc,
    const pacha_eevdf_runqueue *rq) {
  pacha_eevdf_result result;
  result.rc = rc;
  result.rq = *rq;
  return result;
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

static int weighted_delta(int64_t runtime_ns, int64_t weight, int64_t *out) {
  if (!i64_nonnegative(runtime_ns) || !valid_positive(weight)) {
    return 0;
  }
  int64_t quotient = runtime_ns / weight;
  int64_t remainder = runtime_ns % weight;
  int64_t whole;
  if (!checked_mul_i64_nonnegative(
          quotient,
          PACHA_EEVDF_DEFAULT_WEIGHT,
          &whole)) {
    return 0;
  }

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
  return checked_add_i64(whole, fractional, out);
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

static pacha_eevdf_runqueue refresh_runqueue(pacha_eevdf_runqueue rq) {
  int found = 0;
  int64_t min_vruntime = rq.min_vruntime;
  for (size_t i = 0; i < rq.entity_count; ++i) {
    if (!live_for_min(&rq.entities[i])) {
      continue;
    }
    if (!found || rq.entities[i].vruntime < min_vruntime) {
      min_vruntime = rq.entities[i].vruntime;
    }
    found = 1;
  }
  rq.runnable_count = count_runnable(&rq);
  rq.virtual_time = z_max(rq.virtual_time, min_vruntime);
  rq.min_vruntime = min_vruntime;
  return rq;
}

static pacha_eevdf_runqueue recount_runqueue(pacha_eevdf_runqueue rq) {
  rq.runnable_count = count_runnable(&rq);
  return rq;
}

static pacha_eevdf_entity set_entity_state(
    pacha_eevdf_entity entity,
    pacha_eevdf_state state) {
  entity.state = state;
  return entity;
}

static pacha_eevdf_entity place_entity_at_floor(
    pacha_eevdf_entity entity,
    int64_t floor_vruntime) {
  entity.vruntime = z_max(entity.vruntime, floor_vruntime);
  return entity;
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

static int entity_better(
    const pacha_eevdf_entity *candidate,
    const pacha_eevdf_entity *current) {
  if (candidate->deadline < current->deadline) {
    return 1;
  }
  return candidate->deadline == current->deadline &&
         candidate->thread_id < current->thread_id;
}

pacha_eevdf_entity pacha_eevdf_empty_entity(void) {
  pacha_eevdf_entity entity;
  entity.thread_id = PACHA_EEVDF_NO_THREAD_ID;
  entity.generation = 0;
  entity.weight = 0;
  entity.slice_ns = 0;
  entity.service_ns = 0;
  entity.vruntime = 0;
  entity.eligible_time = 0;
  entity.deadline = 0;
  entity.state = PACHA_EEVDF_EMPTY;
  return entity;
}

pacha_eevdf_runqueue pacha_eevdf_empty_runqueue(void) {
  pacha_eevdf_runqueue rq;
  for (size_t i = 0; i < PACHA_EEVDF_MAX_ENTITIES; ++i) {
    rq.entities[i] = pacha_eevdf_empty_entity();
  }
  rq.entity_count = 0;
  rq.runnable_count = 0;
  rq.virtual_time = 0;
  rq.min_vruntime = 0;
  return rq;
}

pacha_eevdf_result pacha_eevdf_reset(const pacha_eevdf_runqueue *rq) {
  (void)rq;
  return ok_result(pacha_eevdf_empty_runqueue());
}

pacha_eevdf_result pacha_eevdf_add(
    const pacha_eevdf_runqueue *rq,
    int64_t thread_id,
    int64_t generation,
    int64_t weight,
    int64_t slice_ns) {
  size_t existing;
  pacha_eevdf_entity entity;
  pacha_eevdf_entity refreshed;
  if (thread_id == PACHA_EEVDF_NO_THREAD_ID ||
      !valid_positive(weight) ||
      !valid_positive(slice_ns)) {
    return fail_result(PACHA_EEVDF_ERR_INVALID, rq);
  }
  if (find_entity_index(rq, thread_id, &existing)) {
    return fail_result(PACHA_EEVDF_ERR_INVALID, rq);
  }
  if (rq->entity_count >= PACHA_EEVDF_MAX_ENTITIES) {
    return fail_result(PACHA_EEVDF_ERR_FULL, rq);
  }
  entity = pacha_eevdf_empty_entity();
  entity.thread_id = thread_id;
  entity.generation = generation;
  entity.weight = weight;
  entity.slice_ns = slice_ns;
  entity.vruntime = rq->min_vruntime;
  entity.eligible_time = rq->min_vruntime;
  entity.deadline = rq->min_vruntime;
  entity.state = PACHA_EEVDF_RUNNABLE;
  if (!refresh_deadline(&entity, rq->min_vruntime, &refreshed)) {
    return fail_result(PACHA_EEVDF_ERR_OVERFLOW, rq);
  }
  pacha_eevdf_runqueue next = *rq;
  next.entities[next.entity_count] = refreshed;
  ++next.entity_count;
  return ok_result(refresh_runqueue(next));
}

pacha_eevdf_result pacha_eevdf_wake(
    const pacha_eevdf_runqueue *rq,
    int64_t thread_id) {
  size_t index;
  pacha_eevdf_entity entity;
  pacha_eevdf_entity refreshed;
  if (!find_entity_index(rq, thread_id, &index)) {
    return fail_result(PACHA_EEVDF_ERR_INVALID, rq);
  }
  entity = rq->entities[index];
  if (entity.state != PACHA_EEVDF_BLOCKED) {
    return fail_result(PACHA_EEVDF_ERR_STATE, rq);
  }
  entity = place_entity_at_floor(entity, rq->min_vruntime);
  entity = set_entity_state(entity, PACHA_EEVDF_RUNNABLE);
  if (!refresh_deadline(&entity, rq->min_vruntime, &refreshed)) {
    return fail_result(PACHA_EEVDF_ERR_OVERFLOW, rq);
  }
  pacha_eevdf_runqueue next = *rq;
  next.entities[index] = refreshed;
  return ok_result(refresh_runqueue(next));
}

pacha_eevdf_result pacha_eevdf_block(
    const pacha_eevdf_runqueue *rq,
    int64_t thread_id) {
  size_t index;
  pacha_eevdf_entity entity;
  if (!find_entity_index(rq, thread_id, &index)) {
    return fail_result(PACHA_EEVDF_ERR_INVALID, rq);
  }
  entity = rq->entities[index];
  if (!runnable_or_running(&entity)) {
    return fail_result(PACHA_EEVDF_ERR_STATE, rq);
  }
  pacha_eevdf_runqueue next = *rq;
  next.entities[index] = set_entity_state(entity, PACHA_EEVDF_BLOCKED);
  return ok_result(refresh_runqueue(next));
}

pacha_eevdf_result pacha_eevdf_exit(
    const pacha_eevdf_runqueue *rq,
    int64_t thread_id) {
  size_t index;
  pacha_eevdf_entity entity;
  if (!find_entity_index(rq, thread_id, &index)) {
    return fail_result(PACHA_EEVDF_ERR_INVALID, rq);
  }
  entity = rq->entities[index];
  if (entity.state == PACHA_EEVDF_EMPTY ||
      entity.state == PACHA_EEVDF_EXITED) {
    return fail_result(PACHA_EEVDF_ERR_STATE, rq);
  }
  pacha_eevdf_runqueue next = *rq;
  next.entities[index] = set_entity_state(entity, PACHA_EEVDF_EXITED);
  return ok_result(refresh_runqueue(next));
}

pacha_eevdf_result pacha_eevdf_charge(
    const pacha_eevdf_runqueue *rq,
    int64_t thread_id,
    int64_t runtime_ns) {
  size_t index;
  int64_t delta;
  int64_t vruntime;
  int64_t service_ns;
  pacha_eevdf_entity entity;
  pacha_eevdf_entity refreshed;
  if (!find_entity_index(rq, thread_id, &index)) {
    return fail_result(PACHA_EEVDF_ERR_INVALID, rq);
  }
  entity = rq->entities[index];
  if (!runnable_or_running(&entity)) {
    return fail_result(PACHA_EEVDF_ERR_STATE, rq);
  }
  if (!weighted_delta(runtime_ns, entity.weight, &delta)) {
    return fail_result(PACHA_EEVDF_ERR_OVERFLOW, rq);
  }
  if (!checked_add_i64(entity.vruntime, delta, &vruntime)) {
    return fail_result(PACHA_EEVDF_ERR_OVERFLOW, rq);
  }
  if (!checked_add_i64(entity.service_ns, runtime_ns, &service_ns)) {
    return fail_result(PACHA_EEVDF_ERR_OVERFLOW, rq);
  }
  entity.service_ns = service_ns;
  entity.vruntime = vruntime;
  if (!refresh_deadline(&entity, rq->min_vruntime, &refreshed)) {
    return fail_result(PACHA_EEVDF_ERR_OVERFLOW, rq);
  }
  pacha_eevdf_runqueue next = *rq;
  next.entities[index] = refreshed;
  return ok_result(refresh_runqueue(next));
}

pacha_eevdf_result pacha_eevdf_mark_running(
    const pacha_eevdf_runqueue *rq,
    int64_t thread_id) {
  size_t index;
  pacha_eevdf_entity entity;
  if (!find_entity_index(rq, thread_id, &index)) {
    return fail_result(PACHA_EEVDF_ERR_INVALID, rq);
  }
  entity = rq->entities[index];
  if (entity.state != PACHA_EEVDF_RUNNABLE) {
    return fail_result(PACHA_EEVDF_ERR_STATE, rq);
  }
  pacha_eevdf_runqueue next = *rq;
  next.entities[index] = set_entity_state(entity, PACHA_EEVDF_RUNNING);
  return ok_result(recount_runqueue(next));
}

pacha_eevdf_result pacha_eevdf_requeue_running(
    const pacha_eevdf_runqueue *rq,
    int64_t thread_id) {
  size_t index;
  pacha_eevdf_entity entity;
  pacha_eevdf_entity refreshed;
  if (!find_entity_index(rq, thread_id, &index)) {
    return fail_result(PACHA_EEVDF_ERR_INVALID, rq);
  }
  entity = rq->entities[index];
  if (entity.state != PACHA_EEVDF_RUNNING) {
    return fail_result(PACHA_EEVDF_ERR_STATE, rq);
  }
  entity = set_entity_state(entity, PACHA_EEVDF_RUNNABLE);
  if (!refresh_deadline(&entity, rq->min_vruntime, &refreshed)) {
    return fail_result(PACHA_EEVDF_ERR_OVERFLOW, rq);
  }
  pacha_eevdf_runqueue next = *rq;
  next.entities[index] = refreshed;
  return ok_result(refresh_runqueue(next));
}

pacha_eevdf_pick_result pacha_eevdf_pick(const pacha_eevdf_runqueue *rq) {
  pacha_eevdf_pick_result result;
  result.rq = *rq;
  result.has_entity = 0;
  result.index = 0;
  result.entity = pacha_eevdf_empty_entity();

  size_t best_index = 0;
  int have_best = 0;
  for (size_t i = 0; i < rq->entity_count; ++i) {
    const pacha_eevdf_entity *entity = &rq->entities[i];
    if (!is_runnable(entity) || entity->eligible_time > rq->virtual_time) {
      continue;
    }
    if (!have_best || entity_better(entity, &result.entity)) {
      result.entity = *entity;
      best_index = i;
      have_best = 1;
    }
  }
  if (have_best) {
    result.has_entity = 1;
    result.index = best_index;
    return result;
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
    return result;
  }

  result.rq.virtual_time = next_time;
  for (size_t i = 0; i < result.rq.entity_count; ++i) {
    const pacha_eevdf_entity *entity = &result.rq.entities[i];
    if (!is_runnable(entity) || entity->eligible_time > next_time) {
      continue;
    }
    if (!result.has_entity || entity_better(entity, &result.entity)) {
      result.entity = *entity;
      result.index = i;
      result.has_entity = 1;
    }
  }
  return result;
}
