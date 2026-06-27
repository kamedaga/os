#include "pacha_eevdf.h"

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

static void test_reset_and_add(void) {
  pacha_eevdf_runqueue rq;
  pacha_eevdf_runqueue next;
  pacha_eevdf_empty_runqueue(&rq);

  pacha_eevdf_rc rc = pacha_eevdf_add(&rq, 42, 7, 1024, 4000000, &next);

  assert(rc == PACHA_EEVDF_OK);
  assert(next.entity_count == 1);
  assert(next.runnable_count == 1);
  assert(next.entities[0].thread_id == 42);
  assert(next.entities[0].generation == 7);
  assert(next.entities[0].vruntime == 0);
  assert(next.entities[0].eligible_time == 0);
  assert(next.entities[0].deadline == 4000000);

  pacha_eevdf_runqueue duplicate;
  rc = pacha_eevdf_add(&next, 42, 8, 1024, 4000000, &duplicate);
  assert(rc == PACHA_EEVDF_ERR_INVALID);
  assert(duplicate.entity_count == next.entity_count);

  pacha_eevdf_runqueue exited;
  rc = pacha_eevdf_exit(&next, 42, &exited);
  assert(rc == PACHA_EEVDF_OK);

  pacha_eevdf_runqueue reused;
  rc = pacha_eevdf_add(&exited, 42, 8, 2048, 1000000, &reused);
  assert(rc == PACHA_EEVDF_OK);
  assert(reused.entity_count == 1);
  assert(reused.runnable_count == 1);
  assert(reused.entities[0].thread_id == 42);
  assert(reused.entities[0].generation == 8);

  pacha_eevdf_runqueue reset;
  rc = pacha_eevdf_reset(&next, &reset);
  assert(rc == PACHA_EEVDF_OK);
  assert(reset.entity_count == 0);
  assert(reset.runnable_count == 0);
}

static void test_pick_tie_breaks_by_thread_id(void) {
  pacha_eevdf_runqueue rq;
  pacha_eevdf_runqueue add_a;
  pacha_eevdf_runqueue add_b;
  pacha_eevdf_empty_runqueue(&rq);

  assert(pacha_eevdf_add(&rq, 20, 1, 1024, 4000000, &add_a) ==
      PACHA_EEVDF_OK);
  assert(pacha_eevdf_add(&add_a, 10, 1, 1024, 4000000, &add_b) ==
      PACHA_EEVDF_OK);

  pacha_eevdf_pick_result pick;
  assert(pacha_eevdf_pick(&add_b, &pick) == PACHA_EEVDF_OK);
  assert(pick.has_entity);
  assert(pick.entity.thread_id == 10);
  assert(pick.index == 1);
}

static void test_mark_requeue_and_charge(void) {
  pacha_eevdf_runqueue rq;
  pacha_eevdf_runqueue add;
  pacha_eevdf_runqueue mark;
  pacha_eevdf_runqueue charge;
  pacha_eevdf_runqueue requeue;
  pacha_eevdf_empty_runqueue(&rq);

  assert(pacha_eevdf_add(&rq, 1, 1, 1024, 4000000, &add) ==
      PACHA_EEVDF_OK);

  assert(pacha_eevdf_mark_running(&add, 1, &mark) == PACHA_EEVDF_OK);
  assert(mark.entities[0].state == PACHA_EEVDF_RUNNING);
  assert(mark.runnable_count == 0);
  assert(mark.min_vruntime == add.min_vruntime);

  assert(pacha_eevdf_charge(&mark, 1, 1000, &charge) == PACHA_EEVDF_OK);
  assert(charge.entities[0].state == PACHA_EEVDF_RUNNING);
  assert(charge.entities[0].service_ns == 1000);
  assert(charge.entities[0].vruntime == 1000);
  assert(charge.min_vruntime == 1000);

  assert(pacha_eevdf_requeue_running(&charge, 1, &requeue) ==
      PACHA_EEVDF_OK);
  assert(requeue.entities[0].state == PACHA_EEVDF_RUNNABLE);
  assert(requeue.runnable_count == 1);
  assert(requeue.entities[0].eligible_time == 1000);
  assert(requeue.entities[0].deadline == 4001000);
}

static void test_block_and_wake_places_at_floor(void) {
  pacha_eevdf_runqueue rq;
  pacha_eevdf_runqueue add_a;
  pacha_eevdf_runqueue add_b;
  pacha_eevdf_runqueue block;
  pacha_eevdf_runqueue mark;
  pacha_eevdf_runqueue charge;
  pacha_eevdf_runqueue wake;
  pacha_eevdf_empty_runqueue(&rq);

  assert(pacha_eevdf_add(&rq, 1, 1, 1024, 4000000, &add_a) ==
      PACHA_EEVDF_OK);
  assert(pacha_eevdf_add(&add_a, 2, 1, 1024, 4000000, &add_b) ==
      PACHA_EEVDF_OK);

  assert(pacha_eevdf_block(&add_b, 1, &block) == PACHA_EEVDF_OK);
  assert(block.entities[0].state == PACHA_EEVDF_BLOCKED);
  assert(block.runnable_count == 1);

  assert(pacha_eevdf_mark_running(&block, 2, &mark) == PACHA_EEVDF_OK);
  assert(pacha_eevdf_charge(&mark, 2, 5000, &charge) == PACHA_EEVDF_OK);
  assert(charge.min_vruntime == 5000);

  assert(pacha_eevdf_wake(&charge, 1, &wake) == PACHA_EEVDF_OK);
  assert(wake.entities[0].state == PACHA_EEVDF_RUNNABLE);
  assert(wake.entities[0].vruntime == 5000);
  assert(wake.entities[0].eligible_time == 5000);
}

static void test_failed_transition_keeps_runqueue(void) {
  pacha_eevdf_runqueue rq;
  pacha_eevdf_runqueue add;
  pacha_eevdf_runqueue failed;
  pacha_eevdf_empty_runqueue(&rq);

  assert(pacha_eevdf_add(&rq, 1, 1, 1024, 4000000, &add) ==
      PACHA_EEVDF_OK);

  assert(pacha_eevdf_wake(&add, 1, &failed) == PACHA_EEVDF_ERR_STATE);
  assert(memcmp(&failed, &add, sizeof(add)) == 0);
}

static void test_table_full_rejects_add(void) {
  pacha_eevdf_runqueue rq;
  pacha_eevdf_empty_runqueue(&rq);

  for (size_t i = 0; i < PACHA_EEVDF_MAX_ENTITIES; ++i) {
    pacha_eevdf_runqueue next;
    assert(pacha_eevdf_add(
        &rq, (int64_t)i + 1, 1, 1024, 4000000, &next) == PACHA_EEVDF_OK);
    rq = next;
  }

  assert(rq.entity_count == PACHA_EEVDF_MAX_ENTITIES);
  pacha_eevdf_runqueue full;
  assert(pacha_eevdf_add(&rq, 1000, 1, 1024, 4000000, &full) ==
      PACHA_EEVDF_ERR_FULL);
  assert(memcmp(&full, &rq, sizeof(rq)) == 0);
}

static void test_exit_compacts_active_range(void) {
  pacha_eevdf_runqueue rq;
  pacha_eevdf_runqueue add_a;
  pacha_eevdf_runqueue add_b;
  pacha_eevdf_runqueue add_c;
  pacha_eevdf_runqueue exited;
  pacha_eevdf_empty_runqueue(&rq);

  assert(pacha_eevdf_add(&rq, 1, 1, 1024, 4000000, &add_a) ==
      PACHA_EEVDF_OK);
  assert(pacha_eevdf_add(&add_a, 2, 1, 1024, 4000000, &add_b) ==
      PACHA_EEVDF_OK);
  assert(pacha_eevdf_add(&add_b, 3, 1, 1024, 4000000, &add_c) ==
      PACHA_EEVDF_OK);

  assert(pacha_eevdf_exit(&add_c, 2, &exited) == PACHA_EEVDF_OK);
  assert(exited.entity_count == 2);
  assert(exited.runnable_count == 2);
  assert(exited.entities[0].thread_id == 1);
  assert(exited.entities[1].thread_id == 3);
  assert(exited.entities[2].state == PACHA_EEVDF_EMPTY);
  assert(exited.entities[2].thread_id == PACHA_EEVDF_NO_THREAD_ID);
}

static void test_charge_overflow_keeps_runqueue(void) {
  pacha_eevdf_runqueue rq;
  pacha_eevdf_runqueue add;
  pacha_eevdf_runqueue overflow;
  pacha_eevdf_empty_runqueue(&rq);
  assert(pacha_eevdf_add(&rq, 1, 1, 1, 4000000, &add) == PACHA_EEVDF_OK);

  assert(pacha_eevdf_charge(&add, 1, INT64_MAX, &overflow) ==
      PACHA_EEVDF_ERR_OVERFLOW);
  assert(memcmp(&overflow, &add, sizeof(add)) == 0);
}

static void test_charge_large_runtime_large_weight(void) {
  pacha_eevdf_runqueue rq;
  pacha_eevdf_runqueue add;
  pacha_eevdf_runqueue charge;
  pacha_eevdf_empty_runqueue(&rq);

  assert(pacha_eevdf_add(&rq, 1, 1, INT64_MAX, 1, &add) == PACHA_EEVDF_OK);

  assert(pacha_eevdf_charge(&add, 1, INT64_MAX, &charge) == PACHA_EEVDF_OK);
  assert(charge.entities[0].service_ns == INT64_MAX);
  assert(charge.entities[0].vruntime == 1024);
  assert(charge.entities[0].deadline == 1025);
}

int main(void) {
  test_reset_and_add();
  test_pick_tie_breaks_by_thread_id();
  test_mark_requeue_and_charge();
  test_block_and_wake_places_at_floor();
  test_failed_transition_keeps_runqueue();
  test_table_full_rejects_add();
  test_exit_compacts_active_range();
  test_charge_overflow_keeps_runqueue();
  test_charge_large_runtime_large_weight();
  return 0;
}
