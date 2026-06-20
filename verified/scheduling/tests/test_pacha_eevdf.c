#include "pacha_eevdf.h"

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

static void test_reset_and_add(void) {
  pacha_eevdf_runqueue rq = pacha_eevdf_empty_runqueue();
  pacha_eevdf_result result =
      pacha_eevdf_add(&rq, 42, 7, 1024, 4000000);

  assert(result.rc == PACHA_EEVDF_OK);
  assert(result.rq.entity_count == 1);
  assert(result.rq.runnable_count == 1);
  assert(result.rq.entities[0].thread_id == 42);
  assert(result.rq.entities[0].generation == 7);
  assert(result.rq.entities[0].vruntime == 0);
  assert(result.rq.entities[0].eligible_time == 0);
  assert(result.rq.entities[0].deadline == 4000000);

  pacha_eevdf_result duplicate =
      pacha_eevdf_add(&result.rq, 42, 8, 1024, 4000000);
  assert(duplicate.rc == PACHA_EEVDF_ERR_INVALID);
  assert(duplicate.rq.entity_count == result.rq.entity_count);

  pacha_eevdf_result reset = pacha_eevdf_reset(&result.rq);
  assert(reset.rc == PACHA_EEVDF_OK);
  assert(reset.rq.entity_count == 0);
  assert(reset.rq.runnable_count == 0);
}

static void test_pick_tie_breaks_by_thread_id(void) {
  pacha_eevdf_runqueue rq = pacha_eevdf_empty_runqueue();
  pacha_eevdf_result add_a =
      pacha_eevdf_add(&rq, 20, 1, 1024, 4000000);
  assert(add_a.rc == PACHA_EEVDF_OK);
  pacha_eevdf_result add_b =
      pacha_eevdf_add(&add_a.rq, 10, 1, 1024, 4000000);
  assert(add_b.rc == PACHA_EEVDF_OK);

  pacha_eevdf_pick_result pick = pacha_eevdf_pick(&add_b.rq);
  assert(pick.has_entity);
  assert(pick.entity.thread_id == 10);
  assert(pick.index == 1);
}

static void test_mark_requeue_and_charge(void) {
  pacha_eevdf_runqueue rq = pacha_eevdf_empty_runqueue();
  pacha_eevdf_result add =
      pacha_eevdf_add(&rq, 1, 1, 1024, 4000000);
  assert(add.rc == PACHA_EEVDF_OK);

  pacha_eevdf_result mark = pacha_eevdf_mark_running(&add.rq, 1);
  assert(mark.rc == PACHA_EEVDF_OK);
  assert(mark.rq.entities[0].state == PACHA_EEVDF_RUNNING);
  assert(mark.rq.runnable_count == 0);
  assert(mark.rq.min_vruntime == add.rq.min_vruntime);

  pacha_eevdf_result charge = pacha_eevdf_charge(&mark.rq, 1, 1000);
  assert(charge.rc == PACHA_EEVDF_OK);
  assert(charge.rq.entities[0].state == PACHA_EEVDF_RUNNING);
  assert(charge.rq.entities[0].service_ns == 1000);
  assert(charge.rq.entities[0].vruntime == 1000);
  assert(charge.rq.min_vruntime == 1000);

  pacha_eevdf_result requeue = pacha_eevdf_requeue_running(&charge.rq, 1);
  assert(requeue.rc == PACHA_EEVDF_OK);
  assert(requeue.rq.entities[0].state == PACHA_EEVDF_RUNNABLE);
  assert(requeue.rq.runnable_count == 1);
  assert(requeue.rq.entities[0].eligible_time == 1000);
  assert(requeue.rq.entities[0].deadline == 4001000);
}

static void test_block_and_wake_places_at_floor(void) {
  pacha_eevdf_runqueue rq = pacha_eevdf_empty_runqueue();
  pacha_eevdf_result add_a =
      pacha_eevdf_add(&rq, 1, 1, 1024, 4000000);
  assert(add_a.rc == PACHA_EEVDF_OK);
  pacha_eevdf_result add_b =
      pacha_eevdf_add(&add_a.rq, 2, 1, 1024, 4000000);
  assert(add_b.rc == PACHA_EEVDF_OK);

  pacha_eevdf_result block = pacha_eevdf_block(&add_b.rq, 1);
  assert(block.rc == PACHA_EEVDF_OK);
  assert(block.rq.entities[0].state == PACHA_EEVDF_BLOCKED);
  assert(block.rq.runnable_count == 1);

  pacha_eevdf_result mark = pacha_eevdf_mark_running(&block.rq, 2);
  assert(mark.rc == PACHA_EEVDF_OK);
  pacha_eevdf_result charge = pacha_eevdf_charge(&mark.rq, 2, 5000);
  assert(charge.rc == PACHA_EEVDF_OK);
  assert(charge.rq.min_vruntime == 5000);

  pacha_eevdf_result wake = pacha_eevdf_wake(&charge.rq, 1);
  assert(wake.rc == PACHA_EEVDF_OK);
  assert(wake.rq.entities[0].state == PACHA_EEVDF_RUNNABLE);
  assert(wake.rq.entities[0].vruntime == 5000);
  assert(wake.rq.entities[0].eligible_time == 5000);
}

static void test_failed_transition_keeps_runqueue(void) {
  pacha_eevdf_runqueue rq = pacha_eevdf_empty_runqueue();
  pacha_eevdf_result add =
      pacha_eevdf_add(&rq, 1, 1, 1024, 4000000);
  assert(add.rc == PACHA_EEVDF_OK);

  pacha_eevdf_result failed = pacha_eevdf_wake(&add.rq, 1);
  assert(failed.rc == PACHA_EEVDF_ERR_STATE);
  assert(memcmp(&failed.rq, &add.rq, sizeof(add.rq)) == 0);
}

static void test_table_full_rejects_add(void) {
  pacha_eevdf_runqueue rq = pacha_eevdf_empty_runqueue();

  for (size_t i = 0; i < PACHA_EEVDF_MAX_ENTITIES; ++i) {
    pacha_eevdf_result add =
        pacha_eevdf_add(&rq, (int64_t)i + 1, 1, 1024, 4000000);
    assert(add.rc == PACHA_EEVDF_OK);
    rq = add.rq;
  }

  assert(rq.entity_count == PACHA_EEVDF_MAX_ENTITIES);
  pacha_eevdf_result full =
      pacha_eevdf_add(&rq, 1000, 1, 1024, 4000000);
  assert(full.rc == PACHA_EEVDF_ERR_FULL);
  assert(memcmp(&full.rq, &rq, sizeof(rq)) == 0);
}

static void test_charge_overflow_keeps_runqueue(void) {
  pacha_eevdf_runqueue rq = pacha_eevdf_empty_runqueue();
  pacha_eevdf_result add = pacha_eevdf_add(&rq, 1, 1, 1, 4000000);
  assert(add.rc == PACHA_EEVDF_OK);

  pacha_eevdf_result overflow =
      pacha_eevdf_charge(&add.rq, 1, INT64_MAX);
  assert(overflow.rc == PACHA_EEVDF_ERR_OVERFLOW);
  assert(memcmp(&overflow.rq, &add.rq, sizeof(add.rq)) == 0);
}

static void test_charge_large_runtime_large_weight(void) {
  pacha_eevdf_runqueue rq = pacha_eevdf_empty_runqueue();
  pacha_eevdf_result add =
      pacha_eevdf_add(&rq, 1, 1, INT64_MAX, 1);
  assert(add.rc == PACHA_EEVDF_OK);

  pacha_eevdf_result charge =
      pacha_eevdf_charge(&add.rq, 1, INT64_MAX);
  assert(charge.rc == PACHA_EEVDF_OK);
  assert(charge.rq.entities[0].service_ns == INT64_MAX);
  assert(charge.rq.entities[0].vruntime == 1024);
  assert(charge.rq.entities[0].deadline == 1025);
}

int main(void) {
  test_reset_and_add();
  test_pick_tie_breaks_by_thread_id();
  test_mark_requeue_and_charge();
  test_block_and_wake_places_at_floor();
  test_failed_transition_keeps_runqueue();
  test_table_full_rejects_add();
  test_charge_overflow_keeps_runqueue();
  test_charge_large_runtime_large_weight();
  return 0;
}
