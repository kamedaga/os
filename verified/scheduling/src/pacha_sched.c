#include "pacha_sched.h"

static pacha_sched_rc map_eevdf_rc(pacha_eevdf_rc rc) {
  switch (rc) {
  case PACHA_EEVDF_OK:
    return PACHA_SCHED_OK;
  case PACHA_EEVDF_ERR_INVALID:
    return PACHA_SCHED_ERR_INVALID;
  case PACHA_EEVDF_ERR_FULL:
    return PACHA_SCHED_ERR_FULL;
  case PACHA_EEVDF_ERR_OVERFLOW:
    return PACHA_SCHED_ERR_OVERFLOW;
  case PACHA_EEVDF_ERR_STATE:
    return PACHA_SCHED_ERR_STATE;
  }
  return PACHA_SCHED_ERR_INVALID;
}

pacha_sched_decision pacha_sched_no_decision(void) {
  pacha_sched_decision decision;
  decision.kind = PACHA_SCHED_DECISION_NONE;
  decision.cpu_id = PACHA_SCHED_NO_CPU;
  decision.thread_id = PACHA_EEVDF_NO_THREAD_ID;
  decision.generation = 0;
  return decision;
}

static pacha_sched_decision run_thread_decision(
    size_t cpu_id,
    const pacha_eevdf_entity *entity) {
  pacha_sched_decision decision;
  decision.kind = PACHA_SCHED_DECISION_RUN_THREAD;
  decision.cpu_id = cpu_id;
  decision.thread_id = entity->thread_id;
  decision.generation = entity->generation;
  return decision;
}

static pacha_sched_decision idle_decision(size_t cpu_id) {
  pacha_sched_decision decision;
  decision.kind = PACHA_SCHED_DECISION_IDLE;
  decision.cpu_id = cpu_id;
  decision.thread_id = PACHA_EEVDF_NO_THREAD_ID;
  decision.generation = 0;
  return decision;
}

static pacha_sched_result sched_result(
    pacha_sched_rc rc,
    pacha_sched_decision decision) {
  pacha_sched_result result;
  result.rc = rc;
  result.decision = decision;
  return result;
}

static int valid_cpu(const pacha_sched_state *sched, size_t cpu_id) {
  return cpu_id < sched->cpu_count;
}

static int cpu_has_current(const pacha_sched_state *sched, size_t cpu_id) {
  return valid_cpu(sched, cpu_id) && sched->cpus[cpu_id].has_current;
}

static void clear_cpu_current(pacha_sched_state *sched, size_t cpu_id) {
  sched->cpus[cpu_id].has_current = 0;
  sched->cpus[cpu_id].current_thread_id = PACHA_EEVDF_NO_THREAD_ID;
}

static void set_cpu_current(
    pacha_sched_state *sched,
    size_t cpu_id,
    int64_t thread_id) {
  sched->cpus[cpu_id].has_current = 1;
  sched->cpus[cpu_id].current_thread_id = thread_id;
}

static int cpu_current_thread_id(
    const pacha_sched_state *sched,
    size_t cpu_id,
    int64_t *thread_id) {
  if (!cpu_has_current(sched, cpu_id)) {
    return 0;
  }
  *thread_id = sched->cpus[cpu_id].current_thread_id;
  return 1;
}

static void clear_current_if_matches(
    pacha_sched_state *sched,
    int64_t thread_id) {
  for (size_t cpu = 0; cpu < sched->cpu_count; ++cpu) {
    if (sched->cpus[cpu].has_current &&
        sched->cpus[cpu].current_thread_id == thread_id) {
      clear_cpu_current(sched, cpu);
    }
  }
}

pacha_sched_state pacha_sched_empty_state(size_t cpu_count) {
  pacha_sched_state sched;
  sched.runqueue = pacha_eevdf_empty_runqueue();
  sched.cpu_count = cpu_count;
  if (sched.cpu_count > PACHA_SCHED_MAX_CPUS) {
    sched.cpu_count = PACHA_SCHED_MAX_CPUS;
  }
  for (size_t i = 0; i < PACHA_SCHED_MAX_CPUS; ++i) {
    sched.cpus[i].has_current = 0;
    sched.cpus[i].current_thread_id = PACHA_EEVDF_NO_THREAD_ID;
  }
  return sched;
}

pacha_sched_result pacha_sched_add_thread(
    pacha_sched_state *sched,
    int64_t thread_id,
    int64_t generation,
    int64_t weight,
    int64_t slice_ns) {
  pacha_eevdf_result result =
      pacha_eevdf_add(&sched->runqueue, thread_id, generation, weight, slice_ns);
  if (result.rc != PACHA_EEVDF_OK) {
    return sched_result(map_eevdf_rc(result.rc), pacha_sched_no_decision());
  }
  sched->runqueue = result.rq;
  return sched_result(PACHA_SCHED_OK, pacha_sched_no_decision());
}

pacha_sched_result pacha_sched_wake_thread(
    pacha_sched_state *sched,
    int64_t thread_id) {
  pacha_eevdf_result result = pacha_eevdf_wake(&sched->runqueue, thread_id);
  if (result.rc != PACHA_EEVDF_OK) {
    return sched_result(map_eevdf_rc(result.rc), pacha_sched_no_decision());
  }
  sched->runqueue = result.rq;
  return sched_result(PACHA_SCHED_OK, pacha_sched_no_decision());
}

pacha_sched_result pacha_sched_block_thread(
    pacha_sched_state *sched,
    int64_t thread_id) {
  pacha_eevdf_result result = pacha_eevdf_block(&sched->runqueue, thread_id);
  if (result.rc != PACHA_EEVDF_OK) {
    return sched_result(map_eevdf_rc(result.rc), pacha_sched_no_decision());
  }
  sched->runqueue = result.rq;
  clear_current_if_matches(sched, thread_id);
  return sched_result(PACHA_SCHED_OK, pacha_sched_no_decision());
}

pacha_sched_result pacha_sched_exit_thread(
    pacha_sched_state *sched,
    int64_t thread_id) {
  pacha_eevdf_result result = pacha_eevdf_exit(&sched->runqueue, thread_id);
  if (result.rc != PACHA_EEVDF_OK) {
    return sched_result(map_eevdf_rc(result.rc), pacha_sched_no_decision());
  }
  sched->runqueue = result.rq;
  clear_current_if_matches(sched, thread_id);
  return sched_result(PACHA_SCHED_OK, pacha_sched_no_decision());
}

pacha_sched_result pacha_sched_on_timer(
    pacha_sched_state *sched,
    size_t cpu_id,
    int64_t runtime_ns) {
  int64_t thread_id;
  if (!valid_cpu(sched, cpu_id)) {
    return sched_result(PACHA_SCHED_ERR_INVALID, pacha_sched_no_decision());
  }
  if (!cpu_current_thread_id(sched, cpu_id, &thread_id)) {
    return sched_result(PACHA_SCHED_OK, pacha_sched_no_decision());
  }
  pacha_eevdf_result result =
      pacha_eevdf_charge(&sched->runqueue, thread_id, runtime_ns);
  if (result.rc != PACHA_EEVDF_OK) {
    return sched_result(map_eevdf_rc(result.rc), pacha_sched_no_decision());
  }
  sched->runqueue = result.rq;
  return sched_result(PACHA_SCHED_OK, pacha_sched_no_decision());
}

pacha_sched_result pacha_sched_pick(
    pacha_sched_state *sched,
    size_t cpu_id) {
  if (!valid_cpu(sched, cpu_id)) {
    return sched_result(PACHA_SCHED_ERR_INVALID, pacha_sched_no_decision());
  }
  if (cpu_has_current(sched, cpu_id)) {
    return sched_result(PACHA_SCHED_ERR_STATE, pacha_sched_no_decision());
  }

  pacha_eevdf_pick_result pick = pacha_eevdf_pick(&sched->runqueue);
  sched->runqueue = pick.rq;
  if (!pick.has_entity) {
    return sched_result(PACHA_SCHED_OK, idle_decision(cpu_id));
  }

  pacha_eevdf_result mark =
      pacha_eevdf_mark_running(&sched->runqueue, pick.entity.thread_id);
  if (mark.rc != PACHA_EEVDF_OK) {
    return sched_result(map_eevdf_rc(mark.rc), pacha_sched_no_decision());
  }
  sched->runqueue = mark.rq;
  set_cpu_current(sched, cpu_id, pick.entity.thread_id);
  return sched_result(PACHA_SCHED_OK, run_thread_decision(cpu_id, &pick.entity));
}

pacha_sched_result pacha_sched_finish_current(
    pacha_sched_state *sched,
    size_t cpu_id) {
  int64_t thread_id;
  if (!valid_cpu(sched, cpu_id)) {
    return sched_result(PACHA_SCHED_ERR_INVALID, pacha_sched_no_decision());
  }
  if (!cpu_current_thread_id(sched, cpu_id, &thread_id)) {
    return sched_result(PACHA_SCHED_OK, pacha_sched_no_decision());
  }
  pacha_eevdf_result result =
      pacha_eevdf_requeue_running(&sched->runqueue, thread_id);
  if (result.rc != PACHA_EEVDF_OK) {
    return sched_result(map_eevdf_rc(result.rc), pacha_sched_no_decision());
  }
  sched->runqueue = result.rq;
  clear_cpu_current(sched, cpu_id);
  return sched_result(PACHA_SCHED_OK, pacha_sched_no_decision());
}
