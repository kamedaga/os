#include "pacha_eevdf.h"

#include <assert.h>
#include <stdint.h>

static uint64_t rng_state = 0x7061636861656575ULL;

static uint64_t next_rand(void) {
  rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
  return rng_state;
}

static int live_for_min(const pacha_eevdf_entity *entity) {
  return entity->state == PACHA_EEVDF_RUNNABLE ||
         entity->state == PACHA_EEVDF_RUNNING;
}

static int is_active_state(pacha_eevdf_state state) {
  return state != PACHA_EEVDF_EMPTY;
}

static int is_runnable(const pacha_eevdf_entity *entity) {
  return entity->state == PACHA_EEVDF_RUNNABLE;
}

static int64_t expected_delta(int64_t runtime_ns, int64_t weight) {
  return (runtime_ns * PACHA_EEVDF_DEFAULT_WEIGHT) / weight;
}

static int64_t expected_slice(int64_t slice_ns, int64_t weight) {
  int64_t delta = expected_delta(slice_ns, weight);
  return delta < 1 ? 1 : delta;
}

static void assert_invariant(const pacha_eevdf_runqueue *rq) {
  assert(rq->entity_count <= PACHA_EEVDF_MAX_ENTITIES);

  size_t runnable_count = 0;
  int found_live = 0;
  int64_t min_vruntime = rq->min_vruntime;
  for (size_t i = 0; i < rq->entity_count; ++i) {
    const pacha_eevdf_entity *entity = &rq->entities[i];
    assert(is_active_state(entity->state));
    assert(entity->thread_id != PACHA_EEVDF_NO_THREAD_ID);
    assert(entity->weight > 0);
    assert(entity->slice_ns > 0);

    for (size_t j = i + 1; j < rq->entity_count; ++j) {
      const pacha_eevdf_entity *other = &rq->entities[j];
      if (is_active_state(other->state)) {
        assert(entity->thread_id != other->thread_id);
      }
    }

    if (live_for_min(entity)) {
      assert(rq->min_vruntime <= entity->vruntime);
      int64_t slice = expected_slice(entity->slice_ns, entity->weight);
      int64_t eligible = entity->vruntime > rq->min_vruntime
          ? entity->vruntime
          : rq->min_vruntime;
      assert(entity->eligible_time == eligible);
      assert(entity->deadline == eligible + slice);
      if (!found_live || entity->vruntime < min_vruntime) {
        min_vruntime = entity->vruntime;
      }
      found_live = 1;
    }

    if (is_runnable(entity)) {
      ++runnable_count;
    }
  }

  for (size_t i = rq->entity_count; i < PACHA_EEVDF_MAX_ENTITIES; ++i) {
    assert(rq->entities[i].state == PACHA_EEVDF_EMPTY);
  }

  assert(rq->runnable_count == runnable_count);
  if (found_live) {
    assert(rq->min_vruntime == min_vruntime);
  }
  assert(rq->virtual_time >= rq->min_vruntime);
}

static int64_t choose_known_thread(const pacha_eevdf_runqueue *rq) {
  if (rq->entity_count == 0) {
    return PACHA_EEVDF_NO_THREAD_ID;
  }
  size_t index = (size_t)(next_rand() % rq->entity_count);
  return rq->entities[index].thread_id;
}

static void apply_success_if_ok(
    pacha_eevdf_runqueue *rq,
    pacha_eevdf_result result) {
  if (result.rc == PACHA_EEVDF_OK) {
    *rq = result.rq;
  }
  assert_invariant(&result.rq);
}

static void run_property_sequence(void) {
  pacha_eevdf_runqueue rq = pacha_eevdf_empty_runqueue();
  assert_invariant(&rq);

  int64_t next_thread_id = 1;
  for (size_t step = 0; step < 2000; ++step) {
    uint64_t op = next_rand() % 8;
    int64_t thread_id = choose_known_thread(&rq);

    switch (op) {
    case 0:
      if (rq.entity_count < 80) {
        apply_success_if_ok(&rq, pacha_eevdf_add(
            &rq,
            next_thread_id++,
            (int64_t)step,
            1 + (int64_t)(next_rand() % 4096),
            1 + (int64_t)(next_rand() % 8000000)));
      }
      break;
    case 1:
      if (thread_id != PACHA_EEVDF_NO_THREAD_ID) {
        apply_success_if_ok(&rq, pacha_eevdf_mark_running(&rq, thread_id));
      }
      break;
    case 2:
      if (thread_id != PACHA_EEVDF_NO_THREAD_ID) {
        apply_success_if_ok(&rq, pacha_eevdf_requeue_running(&rq, thread_id));
      }
      break;
    case 3:
      if (thread_id != PACHA_EEVDF_NO_THREAD_ID) {
        apply_success_if_ok(&rq, pacha_eevdf_block(&rq, thread_id));
      }
      break;
    case 4:
      if (thread_id != PACHA_EEVDF_NO_THREAD_ID) {
        apply_success_if_ok(&rq, pacha_eevdf_wake(&rq, thread_id));
      }
      break;
    case 5:
      if (thread_id != PACHA_EEVDF_NO_THREAD_ID) {
        apply_success_if_ok(&rq, pacha_eevdf_charge(
            &rq,
            thread_id,
            (int64_t)(next_rand() % 100000)));
      }
      break;
    case 6:
      if (thread_id != PACHA_EEVDF_NO_THREAD_ID) {
        apply_success_if_ok(&rq, pacha_eevdf_exit(&rq, thread_id));
      }
      break;
    default: {
      pacha_eevdf_pick_result pick = pacha_eevdf_pick(&rq);
      assert_invariant(&pick.rq);
      if (pick.has_entity) {
        assert(pick.entity.state == PACHA_EEVDF_RUNNABLE);
        assert(pick.index < pick.rq.entity_count);
      }
      rq = pick.rq;
      break;
    }
    }

    assert_invariant(&rq);
  }
}

int main(void) {
  run_property_sequence();
  return 0;
}
