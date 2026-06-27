#include "pacha_kernel_sched.h"

#include <limits.h>

#if PACHA_EEVDF_DEFAULT_WEIGHT != 1024
#error "kernel scheduler migration refresh expects default EEVDF weight 1024"
#endif

static pacha_kernel_sched_rc map_eevdf_rc(pacha_eevdf_rc rc) {
  switch (rc) {
  case PACHA_EEVDF_OK:
    return PACHA_KERNEL_SCHED_OK;
  case PACHA_EEVDF_ERR_INVALID:
    return PACHA_KERNEL_SCHED_ERR_INVALID;
  case PACHA_EEVDF_ERR_FULL:
    return PACHA_KERNEL_SCHED_ERR_FULL;
  case PACHA_EEVDF_ERR_OVERFLOW:
    return PACHA_KERNEL_SCHED_ERR_OVERFLOW;
  case PACHA_EEVDF_ERR_STATE:
    return PACHA_KERNEL_SCHED_ERR_STATE;
  }
  return PACHA_KERNEL_SCHED_ERR_INVALID;
}

static int valid_cpu(const pacha_kernel_sched_state *sched, size_t cpu_id) {
  return cpu_id < sched->cpu_count;
}

static int64_t z_max_i64(int64_t lhs, int64_t rhs) {
  return lhs < rhs ? rhs : lhs;
}

static int checked_add_i64(int64_t lhs, int64_t rhs, int64_t *out) {
  if (rhs > 0 && lhs > INT64_MAX - rhs) {
    return 0;
  }
  if (rhs < 0 && lhs < INT64_MIN - rhs) {
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

static int weighted_fractional_10(
    int64_t remainder,
    int64_t weight,
    int64_t *out) {
  int64_t fractional = 0;
  for (int bit = 0; bit < 10; ++bit) {
    fractional *= 2;
    if (remainder >= weight - remainder) {
      fractional += 1;
      remainder -= weight - remainder;
    } else {
      remainder += remainder;
    }
  }
  *out = fractional;
  return 1;
}

static int weighted_delta(int64_t runtime_ns, int64_t weight, int64_t *out) {
  int64_t whole = 0;
  int64_t fractional = 0;
  int64_t delta = 0;
  if (runtime_ns < 0 || weight <= 0) {
    return 0;
  }
  if (!checked_mul_i64_nonnegative(
          runtime_ns / weight,
          PACHA_EEVDF_DEFAULT_WEIGHT,
          &whole)) {
    return 0;
  }
  if (!weighted_fractional_10(runtime_ns % weight, weight, &fractional)) {
    return 0;
  }
  if (!checked_add_i64(whole, fractional, &delta)) {
    return 0;
  }
  *out = delta;
  return 1;
}

static int weighted_slice(int64_t slice_ns, int64_t weight, int64_t *out) {
  int64_t delta = 0;
  if (!weighted_delta(slice_ns, weight, &delta)) {
    return 0;
  }
  *out = z_max_i64(1, delta);
  return 1;
}

static int refresh_deadline_for_floor(
    const pacha_eevdf_entity *entity,
    int64_t floor_vruntime,
    pacha_eevdf_entity *out) {
  int64_t slice = 0;
  int64_t deadline = 0;
  int64_t eligible = z_max_i64(entity->vruntime, floor_vruntime);
  if (!weighted_slice(entity->slice_ns, entity->weight, &slice)) {
    return 0;
  }
  if (!checked_add_i64(eligible, slice, &deadline)) {
    return 0;
  }
  *out = *entity;
  out->eligible_time = eligible;
  out->deadline = deadline;
  return 1;
}

static int is_active_state(pacha_eevdf_state state) {
  return state != PACHA_EEVDF_EMPTY;
}

static int is_runnable(const pacha_eevdf_entity *entity) {
  return entity->state == PACHA_EEVDF_RUNNABLE;
}

static int is_running(const pacha_eevdf_entity *entity) {
  return entity->state == PACHA_EEVDF_RUNNING;
}

static int runnable_or_running(const pacha_eevdf_entity *entity) {
  return entity->state == PACHA_EEVDF_RUNNABLE ||
         entity->state == PACHA_EEVDF_RUNNING;
}

static int entity_better_values(
    int64_t candidate_deadline,
    int64_t current_deadline,
    int64_t candidate_thread_id,
    int64_t current_thread_id) {
  if (candidate_deadline < current_deadline) {
    return 1;
  }
  if (candidate_deadline == current_deadline) {
    return candidate_thread_id < current_thread_id;
  }
  return 0;
}

static int entity_better(
    const pacha_eevdf_entity *candidate,
    const pacha_eevdf_entity *current) {
  return entity_better_values(
      candidate->deadline,
      current->deadline,
      candidate->thread_id,
      current->thread_id);
}

static int live_for_min(const pacha_eevdf_entity *entity) {
  return entity->state == PACHA_EEVDF_RUNNABLE ||
         entity->state == PACHA_EEVDF_RUNNING;
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

static void refresh_runqueue_metadata(pacha_eevdf_runqueue *rq) {
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
  rq->virtual_time = z_max_i64(rq->virtual_time, min_vruntime);
  rq->min_vruntime = min_vruntime;
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

static int find_entity_cpu(
    const pacha_kernel_sched_state *sched,
    int64_t thread_id,
    size_t *cpu_out) {
  for (size_t cpu_id = 0; cpu_id < sched->cpu_count; ++cpu_id) {
    size_t index = 0;
    if (find_entity_index(&sched->runqueues[cpu_id], thread_id, &index)) {
      *cpu_out = cpu_id;
      return 1;
    }
  }
  return 0;
}

static int current_entity_index(
    const pacha_kernel_sched_state *sched,
    size_t cpu_id,
    size_t *index_out) {
  const pacha_kernel_sched_cpu *cpu = &sched->cpus[cpu_id];
  const pacha_eevdf_runqueue *rq = &sched->runqueues[cpu_id];
  if (!cpu->has_current) {
    return 0;
  }
  for (size_t i = 0; i < rq->entity_count; ++i) {
    const pacha_eevdf_entity *entity = &rq->entities[i];
    if (is_running(entity) &&
        entity->thread_id == cpu->current_thread_id &&
        entity->generation == cpu->current_generation) {
      *index_out = i;
      return 1;
    }
  }
  return 0;
}

void pacha_kernel_sched_no_decision(pacha_kernel_sched_decision *out) {
  out->kind = PACHA_KERNEL_SCHED_DECISION_NONE;
  out->cpu_id = PACHA_KERNEL_SCHED_NO_CPU;
  out->thread_id = PACHA_EEVDF_NO_THREAD_ID;
  out->generation = 0;
  return;
}

static void idle_decision(size_t cpu_id, pacha_kernel_sched_decision *out) {
  out->kind = PACHA_KERNEL_SCHED_DECISION_IDLE;
  out->cpu_id = cpu_id;
  out->thread_id = PACHA_EEVDF_NO_THREAD_ID;
  out->generation = 0;
  return;
}

static void run_thread_decision(
    size_t cpu_id,
    const pacha_eevdf_entity *entity,
    pacha_kernel_sched_decision *out) {
  out->kind = PACHA_KERNEL_SCHED_DECISION_RUN_THREAD;
  out->cpu_id = cpu_id;
  out->thread_id = entity->thread_id;
  out->generation = entity->generation;
  return;
}

static void clear_cpu_current(pacha_kernel_sched_cpu *cpu) {
  cpu->has_current = 0;
  cpu->current_thread_id = PACHA_EEVDF_NO_THREAD_ID;
  cpu->current_generation = 0;
  return;
}

static void set_cpu_current(
    pacha_kernel_sched_cpu *cpu,
    int64_t thread_id,
    int64_t generation) {
  cpu->has_current = 1;
  cpu->current_thread_id = thread_id;
  cpu->current_generation = generation;
  return;
}

static void set_activation_pending(
    pacha_kernel_sched_state *sched,
    size_t cpu_id,
    int pending) {
  sched->cpus[cpu_id].activation_pending = pending ? 1 : 0;
  return;
}

static void clear_pick_result(pacha_eevdf_pick_result *out) {
  out->has_entity = 0;
  out->index = 0;
  pacha_eevdf_empty_entity(&out->entity);
  return;
}

static int find_best_eligible_runnable(
    const pacha_eevdf_runqueue *rq,
    size_t *index_out) {
  size_t best_index = 0;
  int have_best = 0;
  for (size_t i = 0; i < rq->entity_count; ++i) {
    const pacha_eevdf_entity *entity = &rq->entities[i];
    if (!is_runnable(entity) || entity->eligible_time > rq->virtual_time) {
      continue;
    }
    if (!have_best || entity_better(entity, &rq->entities[best_index])) {
      best_index = i;
      have_best = 1;
    }
  }
  if (!have_best) {
    return 0;
  }
  *index_out = best_index;
  return 1;
}

static int find_next_runnable_eligible_time(
    const pacha_eevdf_runqueue *rq,
    int64_t *eligible_time_out) {
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
    return 0;
  }
  *eligible_time_out = next_time;
  return 1;
}

void pacha_kernel_sched_empty_state(
    size_t cpu_count,
    pacha_kernel_sched_state *out) {
  for (size_t i = 0; i < PACHA_KERNEL_SCHED_MAX_CPUS; ++i) {
    pacha_eevdf_empty_runqueue(&out->runqueues[i]);
    out->cpus[i].has_current = 0;
    out->cpus[i].current_thread_id = PACHA_EEVDF_NO_THREAD_ID;
    out->cpus[i].current_generation = 0;
    out->cpus[i].activation_pending = 0;
  }
  out->cpu_count = cpu_count;
  if (out->cpu_count > PACHA_KERNEL_SCHED_MAX_CPUS) {
    out->cpu_count = PACHA_KERNEL_SCHED_MAX_CPUS;
  }
  out->balance_cursor = 0;
  return;
}

pacha_kernel_sched_rc pacha_kernel_sched_add_thread(
    pacha_kernel_sched_state *sched,
    size_t cpu_id,
    int64_t thread_id,
    int64_t generation,
    int64_t weight,
    int64_t slice_ns,
    pacha_kernel_sched_decision *decision_out,
    pacha_eevdf_runqueue *scratch) {
  pacha_kernel_sched_no_decision(decision_out);
  if (!valid_cpu(sched, cpu_id)) {
    return PACHA_KERNEL_SCHED_ERR_INVALID;
  }
  size_t owner_cpu = 0;
  if (find_entity_cpu(sched, thread_id, &owner_cpu)) {
    return PACHA_KERNEL_SCHED_ERR_INVALID;
  }
  pacha_eevdf_rc rc = pacha_eevdf_add(
      &sched->runqueues[cpu_id],
      thread_id,
      generation,
      weight,
      slice_ns,
      scratch);
  if (rc != PACHA_EEVDF_OK) {
    return map_eevdf_rc(rc);
  }
  pacha_eevdf_copy_runqueue(scratch, &sched->runqueues[cpu_id]);
  return PACHA_KERNEL_SCHED_OK;
}

pacha_kernel_sched_rc pacha_kernel_sched_wake_thread(
    pacha_kernel_sched_state *sched,
    int64_t thread_id,
    pacha_kernel_sched_decision *decision_out,
    pacha_eevdf_runqueue *scratch) {
  (void)scratch;
  size_t cpu_id = 0;
  if (!find_entity_cpu(sched, thread_id, &cpu_id)) {
    pacha_kernel_sched_no_decision(decision_out);
    return PACHA_KERNEL_SCHED_ERR_INVALID;
  }
  return pacha_kernel_sched_wake_thread_on_cpu(
      sched,
      cpu_id,
      thread_id,
      decision_out);
}

pacha_kernel_sched_rc pacha_kernel_sched_wake_thread_on_cpu(
    pacha_kernel_sched_state *sched,
    size_t cpu_id,
    int64_t thread_id,
    pacha_kernel_sched_decision *decision_out) {
  pacha_kernel_sched_no_decision(decision_out);
  if (!valid_cpu(sched, cpu_id)) {
    return PACHA_KERNEL_SCHED_ERR_INVALID;
  }
  pacha_eevdf_runqueue *rq = &sched->runqueues[cpu_id];
  size_t index = 0;
  if (!find_entity_index(rq, thread_id, &index)) {
    return PACHA_KERNEL_SCHED_ERR_INVALID;
  }
  pacha_eevdf_entity entity = rq->entities[index];
  if (entity.state != PACHA_EEVDF_BLOCKED) {
    return PACHA_KERNEL_SCHED_ERR_STATE;
  }
  entity.vruntime = z_max_i64(entity.vruntime, rq->min_vruntime);
  entity.state = PACHA_EEVDF_RUNNABLE;
  if (!refresh_deadline_for_floor(&entity, rq->min_vruntime, &entity)) {
    return PACHA_KERNEL_SCHED_ERR_OVERFLOW;
  }
  rq->entities[index] = entity;
  rq->runnable_count += 1;
  refresh_runqueue_metadata(rq);
  return PACHA_KERNEL_SCHED_OK;
}

pacha_kernel_sched_rc pacha_kernel_sched_block_thread(
    pacha_kernel_sched_state *sched,
    int64_t thread_id,
    pacha_kernel_sched_decision *decision_out,
    pacha_eevdf_runqueue *scratch) {
  (void)scratch;
  size_t cpu_id = 0;
  if (!find_entity_cpu(sched, thread_id, &cpu_id)) {
    pacha_kernel_sched_no_decision(decision_out);
    return PACHA_KERNEL_SCHED_ERR_INVALID;
  }
  return pacha_kernel_sched_block_thread_on_cpu(
      sched,
      cpu_id,
      thread_id,
      decision_out);
}

pacha_kernel_sched_rc pacha_kernel_sched_block_thread_on_cpu(
    pacha_kernel_sched_state *sched,
    size_t cpu_id,
    int64_t thread_id,
    pacha_kernel_sched_decision *decision_out) {
  pacha_kernel_sched_no_decision(decision_out);
  if (!valid_cpu(sched, cpu_id)) {
    return PACHA_KERNEL_SCHED_ERR_INVALID;
  }
  pacha_eevdf_runqueue *rq = &sched->runqueues[cpu_id];
  size_t index = 0;
  if (!find_entity_index(rq, thread_id, &index)) {
    return PACHA_KERNEL_SCHED_ERR_INVALID;
  }
  pacha_eevdf_entity *entity = &rq->entities[index];
  if (!runnable_or_running(entity)) {
    return PACHA_KERNEL_SCHED_ERR_STATE;
  }
  if (entity->state == PACHA_EEVDF_RUNNABLE) {
    if (rq->runnable_count == 0) {
      return PACHA_KERNEL_SCHED_ERR_STATE;
    }
    rq->runnable_count -= 1;
  }
  entity->state = PACHA_EEVDF_BLOCKED;
  refresh_runqueue_metadata(rq);
  if (sched->cpus[cpu_id].has_current &&
      sched->cpus[cpu_id].current_thread_id == thread_id) {
    clear_cpu_current(&sched->cpus[cpu_id]);
  }
  return PACHA_KERNEL_SCHED_OK;
}

pacha_kernel_sched_rc pacha_kernel_sched_exit_thread(
    pacha_kernel_sched_state *sched,
    int64_t thread_id,
    pacha_kernel_sched_decision *decision_out,
    pacha_eevdf_runqueue *scratch) {
  (void)scratch;
  size_t cpu_id = 0;
  if (!find_entity_cpu(sched, thread_id, &cpu_id)) {
    pacha_kernel_sched_no_decision(decision_out);
    return PACHA_KERNEL_SCHED_ERR_INVALID;
  }
  return pacha_kernel_sched_exit_thread_on_cpu(
      sched,
      cpu_id,
      thread_id,
      decision_out);
}

pacha_kernel_sched_rc pacha_kernel_sched_exit_thread_on_cpu(
    pacha_kernel_sched_state *sched,
    size_t cpu_id,
    int64_t thread_id,
    pacha_kernel_sched_decision *decision_out) {
  pacha_kernel_sched_no_decision(decision_out);
  if (!valid_cpu(sched, cpu_id)) {
    return PACHA_KERNEL_SCHED_ERR_INVALID;
  }
  pacha_eevdf_runqueue *rq = &sched->runqueues[cpu_id];
  size_t index = 0;
  if (!find_entity_index(rq, thread_id, &index)) {
    return PACHA_KERNEL_SCHED_ERR_INVALID;
  }
  if (rq->entities[index].state == PACHA_EEVDF_EMPTY ||
      rq->entities[index].state == PACHA_EEVDF_EXITED) {
    return PACHA_KERNEL_SCHED_ERR_STATE;
  }
  for (size_t i = index; i + 1 < rq->entity_count; ++i) {
    rq->entities[i] = rq->entities[i + 1];
  }
  --rq->entity_count;
  pacha_eevdf_empty_entity(&rq->entities[rq->entity_count]);
  refresh_runqueue_metadata(rq);
  if (sched->cpus[cpu_id].has_current &&
      sched->cpus[cpu_id].current_thread_id == thread_id) {
    clear_cpu_current(&sched->cpus[cpu_id]);
  }
  return PACHA_KERNEL_SCHED_OK;
}

pacha_kernel_sched_rc pacha_kernel_sched_on_timer(
    pacha_kernel_sched_state *sched,
    size_t cpu_id,
    int64_t runtime_ns,
    pacha_kernel_sched_decision *decision_out,
    pacha_eevdf_runqueue *scratch) {
  pacha_kernel_sched_no_decision(decision_out);
  if (!valid_cpu(sched, cpu_id)) {
    return PACHA_KERNEL_SCHED_ERR_INVALID;
  }
  if (!sched->cpus[cpu_id].has_current) {
    return PACHA_KERNEL_SCHED_OK;
  }
  size_t current_index = 0;
  if (!current_entity_index(sched, cpu_id, &current_index)) {
    clear_cpu_current(&sched->cpus[cpu_id]);
    return PACHA_KERNEL_SCHED_OK;
  }
  pacha_eevdf_rc rc = pacha_eevdf_charge(
      &sched->runqueues[cpu_id],
      sched->runqueues[cpu_id].entities[current_index].thread_id,
      runtime_ns,
      scratch);
  if (rc != PACHA_EEVDF_OK) {
    return map_eevdf_rc(rc);
  }
  pacha_eevdf_copy_runqueue(scratch, &sched->runqueues[cpu_id]);
  return PACHA_KERNEL_SCHED_OK;
}

pacha_kernel_sched_rc pacha_kernel_sched_pick_cpu(
    pacha_kernel_sched_state *sched,
    size_t cpu_id,
    pacha_kernel_sched_decision *decision_out,
    pacha_eevdf_pick_result *pick_scratch,
    pacha_eevdf_runqueue *scratch) {
  (void)scratch;
  pacha_kernel_sched_no_decision(decision_out);
  clear_pick_result(pick_scratch);
  if (!valid_cpu(sched, cpu_id)) {
    return PACHA_KERNEL_SCHED_ERR_INVALID;
  }
  if (sched->cpus[cpu_id].has_current) {
    return PACHA_KERNEL_SCHED_ERR_STATE;
  }

  pacha_eevdf_runqueue *rq = &sched->runqueues[cpu_id];
  size_t best_index = 0;
  if (!find_best_eligible_runnable(rq, &best_index)) {
    int64_t next_time = 0;
    if (!find_next_runnable_eligible_time(rq, &next_time)) {
      set_activation_pending(sched, cpu_id, 0);
      idle_decision(cpu_id, decision_out);
      return PACHA_KERNEL_SCHED_OK;
    }
    rq->virtual_time = next_time;
    if (!find_best_eligible_runnable(rq, &best_index)) {
      return PACHA_KERNEL_SCHED_ERR_STATE;
    }
  }

  pacha_eevdf_entity *entity = &rq->entities[best_index];
  if (entity->state != PACHA_EEVDF_RUNNABLE) {
    return PACHA_KERNEL_SCHED_ERR_STATE;
  }
  pick_scratch->has_entity = 1;
  pick_scratch->index = best_index;
  pick_scratch->entity = *entity;
  entity->state = PACHA_EEVDF_RUNNING;
  if (rq->runnable_count == 0) {
    return PACHA_KERNEL_SCHED_ERR_STATE;
  }
  rq->runnable_count -= 1;

  set_activation_pending(sched, cpu_id, 0);
  set_cpu_current(
      &sched->cpus[cpu_id],
      pick_scratch->entity.thread_id,
      pick_scratch->entity.generation);
  run_thread_decision(cpu_id, &pick_scratch->entity, decision_out);
  return PACHA_KERNEL_SCHED_OK;
}

pacha_kernel_sched_rc pacha_kernel_sched_finish_current(
    pacha_kernel_sched_state *sched,
    size_t cpu_id,
    pacha_kernel_sched_decision *decision_out,
    pacha_eevdf_runqueue *scratch) {
  (void)scratch;
  pacha_kernel_sched_no_decision(decision_out);
  if (!valid_cpu(sched, cpu_id)) {
    return PACHA_KERNEL_SCHED_ERR_INVALID;
  }
  if (!sched->cpus[cpu_id].has_current) {
    return PACHA_KERNEL_SCHED_OK;
  }
  size_t current_index = 0;
  if (!current_entity_index(sched, cpu_id, &current_index)) {
    clear_cpu_current(&sched->cpus[cpu_id]);
    return PACHA_KERNEL_SCHED_OK;
  }
  pacha_eevdf_entity *entity =
      &sched->runqueues[cpu_id].entities[current_index];
  if (entity->state != PACHA_EEVDF_RUNNING) {
    return PACHA_KERNEL_SCHED_ERR_STATE;
  }
  entity->state = PACHA_EEVDF_RUNNABLE;
  sched->runqueues[cpu_id].runnable_count += 1;
  clear_cpu_current(&sched->cpus[cpu_id]);
  return PACHA_KERNEL_SCHED_OK;
}

pacha_kernel_sched_rc pacha_kernel_sched_handoff_to_thread_on_cpu(
    pacha_kernel_sched_state *sched,
    size_t cpu_id,
    int64_t thread_id,
    pacha_kernel_sched_decision *decision_out,
    pacha_eevdf_runqueue *scratch) {
  (void)scratch;
  pacha_kernel_sched_no_decision(decision_out);
  if (!valid_cpu(sched, cpu_id)) {
    return PACHA_KERNEL_SCHED_ERR_INVALID;
  }
  if (!sched->cpus[cpu_id].has_current) {
    return PACHA_KERNEL_SCHED_ERR_STATE;
  }

  pacha_eevdf_runqueue *rq = &sched->runqueues[cpu_id];
  size_t current_index = 0;
  if (!current_entity_index(sched, cpu_id, &current_index)) {
    return PACHA_KERNEL_SCHED_ERR_STATE;
  }

  size_t target_index = 0;
  if (!find_entity_index(rq, thread_id, &target_index)) {
    return PACHA_KERNEL_SCHED_ERR_INVALID;
  }
  pacha_eevdf_entity *current = &rq->entities[current_index];
  pacha_eevdf_entity *target = &rq->entities[target_index];
  if (current->state != PACHA_EEVDF_RUNNING ||
      target->state != PACHA_EEVDF_RUNNABLE) {
    return PACHA_KERNEL_SCHED_ERR_STATE;
  }

  current->state = PACHA_EEVDF_RUNNABLE;
  target->state = PACHA_EEVDF_RUNNING;
  set_cpu_current(&sched->cpus[cpu_id], target->thread_id, target->generation);
  run_thread_decision(cpu_id, target, decision_out);
  refresh_runqueue_metadata(rq);
  return PACHA_KERNEL_SCHED_OK;
}

pacha_kernel_sched_rc pacha_kernel_sched_request_activation(
    pacha_kernel_sched_state *sched,
    size_t cpu_id,
    pacha_kernel_sched_decision *decision_out) {
  pacha_kernel_sched_no_decision(decision_out);
  if (!valid_cpu(sched, cpu_id)) {
    return PACHA_KERNEL_SCHED_ERR_INVALID;
  }
  set_activation_pending(sched, cpu_id, 1);
  return PACHA_KERNEL_SCHED_OK;
}

pacha_kernel_sched_rc pacha_kernel_sched_claim_activation(
    pacha_kernel_sched_state *sched,
    size_t cpu_id,
    pacha_kernel_sched_decision *decision_out,
    pacha_eevdf_pick_result *pick_scratch,
    pacha_eevdf_runqueue *scratch) {
  if (!valid_cpu(sched, cpu_id)) {
    pacha_kernel_sched_no_decision(decision_out);
    return PACHA_KERNEL_SCHED_ERR_INVALID;
  }
  return pacha_kernel_sched_pick_cpu(
      sched,
      cpu_id,
      decision_out,
      pick_scratch,
      scratch);
}

static void remove_entity_from_runqueue(
    const pacha_eevdf_runqueue *src,
    int64_t thread_id,
    pacha_eevdf_runqueue *out) {
  size_t out_index = 0;
  pacha_eevdf_empty_runqueue(out);
  out->virtual_time = src->virtual_time;
  out->min_vruntime = src->min_vruntime;
  for (size_t i = 0; i < src->entity_count; ++i) {
    if (src->entities[i].thread_id == thread_id) {
      continue;
    }
    out->entities[out_index] = src->entities[i];
    ++out_index;
  }
  out->entity_count = out_index;
  refresh_runqueue_metadata(out);
  return;
}

static int append_migrated_entity(
    const pacha_eevdf_runqueue *dst,
    const pacha_eevdf_entity *entity,
    pacha_eevdf_runqueue *out) {
  pacha_eevdf_entity moved = *entity;
  moved.state = PACHA_EEVDF_RUNNABLE;
  moved.vruntime = z_max_i64(entity->vruntime, dst->min_vruntime);
  if (!refresh_deadline_for_floor(&moved, dst->min_vruntime, &moved)) {
    return 0;
  }
  *out = *dst;
  out->entities[out->entity_count] = moved;
  ++out->entity_count;
  refresh_runqueue_metadata(out);
  return 1;
}

pacha_kernel_sched_rc pacha_kernel_sched_migrate_runnable(
    pacha_kernel_sched_state *sched,
    size_t src_cpu,
    size_t dst_cpu,
    int64_t thread_id,
    pacha_kernel_sched_decision *decision_out,
    pacha_eevdf_runqueue *src_scratch,
    pacha_eevdf_runqueue *dst_scratch) {
  size_t index = 0;
  pacha_eevdf_entity entity;
  pacha_kernel_sched_no_decision(decision_out);
  if (!valid_cpu(sched, src_cpu) || !valid_cpu(sched, dst_cpu)) {
    return PACHA_KERNEL_SCHED_ERR_INVALID;
  }
  if (src_cpu == dst_cpu) {
    return PACHA_KERNEL_SCHED_ERR_INVALID;
  }
  if (!find_entity_index(&sched->runqueues[src_cpu], thread_id, &index)) {
    return PACHA_KERNEL_SCHED_ERR_INVALID;
  }
  entity = sched->runqueues[src_cpu].entities[index];
  if (entity.state != PACHA_EEVDF_RUNNABLE) {
    return PACHA_KERNEL_SCHED_ERR_STATE;
  }
  if (sched->runqueues[dst_cpu].entity_count >= PACHA_EEVDF_MAX_ENTITIES) {
    return PACHA_KERNEL_SCHED_ERR_FULL;
  }
  if (find_entity_index(&sched->runqueues[dst_cpu], thread_id, &index)) {
    return PACHA_KERNEL_SCHED_ERR_INVALID;
  }
  if (!append_migrated_entity(
          &sched->runqueues[dst_cpu],
          &entity,
          dst_scratch)) {
    return PACHA_KERNEL_SCHED_ERR_OVERFLOW;
  }
  remove_entity_from_runqueue(
      &sched->runqueues[src_cpu],
      thread_id,
      src_scratch);
  pacha_eevdf_copy_runqueue(src_scratch, &sched->runqueues[src_cpu]);
  pacha_eevdf_copy_runqueue(dst_scratch, &sched->runqueues[dst_cpu]);
  return PACHA_KERNEL_SCHED_OK;
}

static int validate_runqueue(const pacha_eevdf_runqueue *rq) {
  size_t runnable_count = 0;
  int have_live = 0;
  int have_min = 0;
  int64_t min_vruntime = rq->min_vruntime;
  if (rq->entity_count > PACHA_EEVDF_MAX_ENTITIES) {
    return 0;
  }
  if (rq->virtual_time < rq->min_vruntime) {
    return 0;
  }
  for (size_t i = 0; i < PACHA_EEVDF_MAX_ENTITIES; ++i) {
    const pacha_eevdf_entity *entity = &rq->entities[i];
    if (i >= rq->entity_count) {
      if (entity->state != PACHA_EEVDF_EMPTY) {
        return 0;
      }
      continue;
    }
    if (is_active_state(entity->state)) {
      if (entity->weight <= 0 || entity->slice_ns <= 0) {
        return 0;
      }
      if (entity->thread_id == PACHA_EEVDF_NO_THREAD_ID) {
        return 0;
      }
    }
    if (is_runnable(entity)) {
      ++runnable_count;
    }
    if (live_for_min(entity)) {
      int64_t slice = 0;
      int64_t expected_eligible = z_max_i64(entity->vruntime, rq->min_vruntime);
      int64_t expected_deadline = 0;
      if (!weighted_slice(entity->slice_ns, entity->weight, &slice)) {
        return 0;
      }
      if (!checked_add_i64(expected_eligible, slice, &expected_deadline)) {
        return 0;
      }
      if (entity->eligible_time != expected_eligible ||
          entity->deadline != expected_deadline ||
          rq->min_vruntime > entity->vruntime) {
        return 0;
      }
      if (!have_live || entity->vruntime < min_vruntime) {
        min_vruntime = entity->vruntime;
      }
      if (entity->vruntime == rq->min_vruntime) {
        have_min = 1;
      }
      have_live = 1;
    }
    for (size_t j = i + 1; j < rq->entity_count; ++j) {
      const pacha_eevdf_entity *other = &rq->entities[j];
      if (entity->thread_id == other->thread_id &&
          entity->thread_id != PACHA_EEVDF_NO_THREAD_ID &&
          is_active_state(entity->state) &&
          is_active_state(other->state)) {
        return 0;
      }
    }
  }
  if (rq->runnable_count != runnable_count) {
    return 0;
  }
  if (have_live && (!have_min || min_vruntime != rq->min_vruntime)) {
    return 0;
  }
  return 1;
}

static int cpu_has_running_entity(
    const pacha_kernel_sched_state *sched,
    size_t cpu_id,
    int64_t thread_id,
    int64_t generation) {
  const pacha_eevdf_runqueue *rq = &sched->runqueues[cpu_id];
  for (size_t i = 0; i < rq->entity_count; ++i) {
    const pacha_eevdf_entity *entity = &rq->entities[i];
    if (is_running(entity) &&
        entity->thread_id == thread_id &&
        entity->generation == generation) {
      return 1;
    }
  }
  return 0;
}

pacha_kernel_sched_rc pacha_kernel_sched_validate(
    const pacha_kernel_sched_state *sched) {
  if (sched->cpu_count > PACHA_KERNEL_SCHED_MAX_CPUS) {
    return PACHA_KERNEL_SCHED_ERR_STATE;
  }
  for (size_t cpu_id = 0; cpu_id < PACHA_KERNEL_SCHED_MAX_CPUS; ++cpu_id) {
    const pacha_kernel_sched_cpu *cpu = &sched->cpus[cpu_id];
    if (cpu->activation_pending && cpu_id >= sched->cpu_count) {
      return PACHA_KERNEL_SCHED_ERR_STATE;
    }
  }
  for (size_t cpu_id = 0; cpu_id < sched->cpu_count; ++cpu_id) {
    const pacha_eevdf_runqueue *rq = &sched->runqueues[cpu_id];
    const pacha_kernel_sched_cpu *cpu = &sched->cpus[cpu_id];
    if (!validate_runqueue(rq)) {
      return PACHA_KERNEL_SCHED_ERR_STATE;
    }
    if (cpu->has_current) {
      if (cpu->current_thread_id == PACHA_EEVDF_NO_THREAD_ID ||
          !cpu_has_running_entity(
              sched,
              cpu_id,
              cpu->current_thread_id,
              cpu->current_generation)) {
        return PACHA_KERNEL_SCHED_ERR_STATE;
      }
    }
    for (size_t i = 0; i < rq->entity_count; ++i) {
      const pacha_eevdf_entity *entity = &rq->entities[i];
      if (!is_running(entity)) {
        continue;
      }
      if (!cpu->has_current ||
          cpu->current_thread_id != entity->thread_id ||
          cpu->current_generation != entity->generation) {
        return PACHA_KERNEL_SCHED_ERR_STATE;
      }
    }
    for (size_t other_cpu_id = cpu_id + 1;
         other_cpu_id < sched->cpu_count;
         ++other_cpu_id) {
      const pacha_eevdf_runqueue *other_rq =
          &sched->runqueues[other_cpu_id];
      const pacha_kernel_sched_cpu *other_cpu =
          &sched->cpus[other_cpu_id];
      if (cpu->has_current &&
          other_cpu->has_current &&
          cpu->current_thread_id != PACHA_EEVDF_NO_THREAD_ID &&
          cpu->current_thread_id == other_cpu->current_thread_id) {
        return PACHA_KERNEL_SCHED_ERR_STATE;
      }
      for (size_t i = 0; i < rq->entity_count; ++i) {
        const pacha_eevdf_entity *entity = &rq->entities[i];
        if (!is_active_state(entity->state) ||
            entity->thread_id == PACHA_EEVDF_NO_THREAD_ID) {
          continue;
        }
        for (size_t j = 0; j < other_rq->entity_count; ++j) {
          const pacha_eevdf_entity *other = &other_rq->entities[j];
          if (is_active_state(other->state) &&
              entity->thread_id == other->thread_id) {
            return PACHA_KERNEL_SCHED_ERR_STATE;
          }
        }
      }
    }
  }
  return PACHA_KERNEL_SCHED_OK;
}
