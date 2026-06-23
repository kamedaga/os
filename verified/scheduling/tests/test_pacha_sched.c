#include "pacha_sched.h"

#include <assert.h>

static void test_write_no_decision(void) {
  pacha_sched_decision decision;
  pacha_sched_no_decision(&decision);
  assert(decision.kind == PACHA_SCHED_DECISION_NONE);
  assert(decision.cpu_id == PACHA_SCHED_NO_CPU);
  assert(decision.thread_id == PACHA_EEVDF_NO_THREAD_ID);
  assert(decision.generation == 0);
}

static void test_pick_and_finish_current(void) {
  pacha_sched_state sched;
  pacha_sched_decision decision;
  pacha_eevdf_runqueue scratch;
  pacha_eevdf_pick_result pick_scratch;
  pacha_sched_empty_state(2, &sched);

  assert(pacha_sched_add_thread(
      &sched, 20, 1, 1024, 4000000, &decision, &scratch) ==
      PACHA_SCHED_OK);
  assert(decision.kind == PACHA_SCHED_DECISION_NONE);
  assert(pacha_sched_add_thread(
      &sched, 10, 1, 1024, 4000000, &decision, &scratch) ==
      PACHA_SCHED_OK);

  assert(pacha_sched_pick(&sched, 0, &decision, &pick_scratch, &scratch) ==
      PACHA_SCHED_OK);
  assert(decision.kind == PACHA_SCHED_DECISION_RUN_THREAD);
  assert(decision.cpu_id == 0);
  assert(decision.thread_id == 10);
  assert(decision.generation == 1);
  assert(sched.cpus[0].has_current);
  assert(sched.cpus[0].current_thread_id == 10);
  assert(sched.cpus[0].current_generation == 1);
  assert(sched.runqueue.runnable_count == 1);

  assert(pacha_sched_pick(&sched, 0, &decision, &pick_scratch, &scratch) ==
      PACHA_SCHED_ERR_STATE);

  assert(pacha_sched_on_timer(&sched, 0, 1000, &decision, &scratch) ==
      PACHA_SCHED_OK);

  assert(pacha_sched_finish_current(&sched, 0, &decision, &scratch) ==
      PACHA_SCHED_OK);
  assert(!sched.cpus[0].has_current);
  assert(sched.runqueue.runnable_count == 2);
}

static void test_idle_and_invalid_cpu(void) {
  pacha_sched_state sched;
  pacha_sched_decision decision;
  pacha_eevdf_runqueue scratch;
  pacha_eevdf_pick_result pick_scratch;
  pacha_sched_empty_state(1, &sched);

  assert(pacha_sched_pick(&sched, 1, &decision, &pick_scratch, &scratch) ==
      PACHA_SCHED_ERR_INVALID);

  assert(pacha_sched_pick(&sched, 0, &decision, &pick_scratch, &scratch) ==
      PACHA_SCHED_OK);
  assert(decision.kind == PACHA_SCHED_DECISION_IDLE);
  assert(decision.cpu_id == 0);
  assert(!sched.cpus[0].has_current);
}

static void test_block_wake_exit_lifecycle(void) {
  pacha_sched_state sched;
  pacha_sched_decision decision;
  pacha_eevdf_runqueue scratch;
  pacha_eevdf_pick_result pick_scratch;
  pacha_sched_empty_state(1, &sched);
  assert(pacha_sched_add_thread(
      &sched, 1, 7, 1024, 4000000, &decision, &scratch) ==
      PACHA_SCHED_OK);

  assert(pacha_sched_pick(&sched, 0, &decision, &pick_scratch, &scratch) ==
      PACHA_SCHED_OK);
  assert(decision.thread_id == 1);

  assert(pacha_sched_block_thread(&sched, 1, &decision, &scratch) ==
      PACHA_SCHED_OK);
  assert(!sched.cpus[0].has_current);
  assert(sched.runqueue.runnable_count == 0);

  assert(pacha_sched_pick(&sched, 0, &decision, &pick_scratch, &scratch) ==
      PACHA_SCHED_OK);
  assert(decision.kind == PACHA_SCHED_DECISION_IDLE);

  assert(pacha_sched_wake_thread(&sched, 1, &decision, &scratch) ==
      PACHA_SCHED_OK);
  assert(sched.runqueue.runnable_count == 1);

  assert(pacha_sched_pick(&sched, 0, &decision, &pick_scratch, &scratch) ==
      PACHA_SCHED_OK);
  assert(decision.kind == PACHA_SCHED_DECISION_RUN_THREAD);
  assert(decision.thread_id == 1);

  assert(pacha_sched_exit_thread(&sched, 1, &decision, &scratch) ==
      PACHA_SCHED_OK);
  assert(!sched.cpus[0].has_current);
  assert(sched.runqueue.runnable_count == 0);
}

static void test_readd_exited_thread_slot(void) {
  pacha_sched_state sched;
  pacha_sched_decision decision;
  pacha_eevdf_runqueue scratch;
  pacha_eevdf_pick_result pick_scratch;
  pacha_sched_empty_state(1, &sched);

  assert(pacha_sched_add_thread(
      &sched, 1, 7, 1024, 4000000, &decision, &scratch) ==
      PACHA_SCHED_OK);
  assert(pacha_sched_exit_thread(&sched, 1, &decision, &scratch) ==
      PACHA_SCHED_OK);
  assert(pacha_sched_add_thread(
      &sched, 1, 8, 2048, 1000000, &decision, &scratch) ==
      PACHA_SCHED_OK);
  assert(sched.runqueue.entity_count == 1);
  assert(sched.runqueue.runnable_count == 1);

  assert(pacha_sched_pick(&sched, 0, &decision, &pick_scratch, &scratch) ==
      PACHA_SCHED_OK);
  assert(decision.kind == PACHA_SCHED_DECISION_RUN_THREAD);
  assert(decision.thread_id == 1);
  assert(decision.generation == 8);
}

static void test_stale_current_generation_does_not_charge_reused_slot(void) {
  pacha_sched_state sched;
  pacha_sched_decision decision;
  pacha_eevdf_runqueue scratch;
  pacha_eevdf_pick_result pick_scratch;
  pacha_sched_empty_state(1, &sched);

  assert(pacha_sched_add_thread(
      &sched, 1, 7, 1024, 4000000, &decision, &scratch) ==
      PACHA_SCHED_OK);
  assert(pacha_sched_pick(&sched, 0, &decision, &pick_scratch, &scratch) ==
      PACHA_SCHED_OK);
  assert(decision.thread_id == 1);
  assert(decision.generation == 7);

  assert(pacha_sched_exit_thread(&sched, 1, &decision, &scratch) ==
      PACHA_SCHED_OK);
  sched.cpus[0].has_current = 1;
  sched.cpus[0].current_thread_id = 1;
  sched.cpus[0].current_generation = 7;

  assert(pacha_sched_add_thread(
      &sched, 1, 8, 1024, 4000000, &decision, &scratch) ==
      PACHA_SCHED_OK);
  assert(pacha_sched_on_timer(&sched, 0, 1000, &decision, &scratch) ==
      PACHA_SCHED_OK);
  assert(!sched.cpus[0].has_current);
  assert(sched.runqueue.entities[0].generation == 8);
  assert(sched.runqueue.entities[0].service_ns == 0);

  assert(pacha_sched_pick(&sched, 0, &decision, &pick_scratch, &scratch) ==
      PACHA_SCHED_OK);
  assert(decision.thread_id == 1);
  assert(decision.generation == 8);
}

int main(void) {
  test_write_no_decision();
  test_pick_and_finish_current();
  test_idle_and_invalid_cpu();
  test_block_wake_exit_lifecycle();
  test_readd_exited_thread_slot();
  test_stale_current_generation_does_not_charge_reused_slot();
  return 0;
}
