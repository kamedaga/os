#include "pacha_kernel_sched.h"

#include <assert.h>

static void assert_valid(const pacha_kernel_sched_state *sched) {
  assert(pacha_kernel_sched_validate(sched) == PACHA_KERNEL_SCHED_OK);
}

static const pacha_eevdf_entity *find_entity(
    const pacha_eevdf_runqueue *rq,
    int64_t thread_id) {
  for (size_t i = 0; i < rq->entity_count; ++i) {
    if (rq->entities[i].thread_id == thread_id) {
      return &rq->entities[i];
    }
  }
  return NULL;
}

static void test_empty_state_clamps_cpus(void) {
  pacha_kernel_sched_state sched;
  pacha_kernel_sched_decision decision;
  pacha_kernel_sched_empty_state(PACHA_KERNEL_SCHED_MAX_CPUS + 7, &sched);
  assert_valid(&sched);
  assert(sched.cpu_count == PACHA_KERNEL_SCHED_MAX_CPUS);
  assert(sched.balance_cursor == 0);
  for (size_t i = 0; i < sched.cpu_count; ++i) {
    assert(sched.runqueues[i].entity_count == 0);
    assert(!sched.cpus[i].has_current);
    assert(!sched.cpus[i].activation_pending);
  }

  pacha_kernel_sched_no_decision(&decision);
  assert(decision.kind == PACHA_KERNEL_SCHED_DECISION_NONE);
  assert(decision.cpu_id == PACHA_KERNEL_SCHED_NO_CPU);
}

static void test_per_cpu_pick_and_finish(void) {
  pacha_kernel_sched_state sched;
  pacha_kernel_sched_decision decision;
  pacha_eevdf_runqueue scratch;
  pacha_eevdf_pick_result pick;
  const pacha_eevdf_entity *charged = NULL;
  pacha_kernel_sched_empty_state(2, &sched);
  assert_valid(&sched);

  assert(pacha_kernel_sched_add_thread(
      &sched, 1, 20, 1, 1024, 4000000, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);
  assert(pacha_kernel_sched_add_thread(
      &sched, 1, 10, 1, 1024, 4000000, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);
  assert(sched.runqueues[0].runnable_count == 0);
  assert(sched.runqueues[1].runnable_count == 2);

  assert(pacha_kernel_sched_pick_cpu(&sched, 0, &decision, &pick, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);
  assert(decision.kind == PACHA_KERNEL_SCHED_DECISION_IDLE);
  assert(decision.cpu_id == 0);

  assert(pacha_kernel_sched_pick_cpu(&sched, 1, &decision, &pick, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);
  assert(decision.kind == PACHA_KERNEL_SCHED_DECISION_RUN_THREAD);
  assert(decision.cpu_id == 1);
  assert(decision.thread_id == 10);
  assert(sched.cpus[1].has_current);
  assert(sched.cpus[1].current_thread_id == 10);
  assert(sched.runqueues[1].runnable_count == 1);

  assert(pacha_kernel_sched_pick_cpu(&sched, 1, &decision, &pick, &scratch) ==
      PACHA_KERNEL_SCHED_ERR_STATE);

  assert(pacha_kernel_sched_on_timer(&sched, 1, 1000, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);
  assert(sched.cpus[1].has_current);
  assert(sched.cpus[1].current_thread_id == 10);
  assert(sched.cpus[1].current_generation == 1);
  charged = find_entity(&sched.runqueues[1], 10);
  assert(charged != NULL);
  assert(charged->generation == 1);
  assert(charged->state == PACHA_EEVDF_RUNNING);
  assert(charged->service_ns == 1000);
  assert(pacha_kernel_sched_finish_current(&sched, 1, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);
  assert(!sched.cpus[1].has_current);
  assert(sched.runqueues[1].runnable_count == 2);
}

static void test_activation_claim_clears_pending(void) {
  pacha_kernel_sched_state sched;
  pacha_kernel_sched_decision decision;
  pacha_eevdf_runqueue scratch;
  pacha_eevdf_pick_result pick;
  pacha_kernel_sched_empty_state(1, &sched);
  assert_valid(&sched);

  assert(pacha_kernel_sched_request_activation(&sched, 0, &decision) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);
  assert(sched.cpus[0].activation_pending);

  assert(pacha_kernel_sched_add_thread(
      &sched, 0, 7, 2, 1024, 4000000, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);
  assert(pacha_kernel_sched_claim_activation(
      &sched, 0, &decision, &pick, &scratch) == PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);
  assert(decision.kind == PACHA_KERNEL_SCHED_DECISION_RUN_THREAD);
  assert(decision.thread_id == 7);
  assert(!sched.cpus[0].activation_pending);
}

static void test_handoff_to_woken_thread_on_cpu(void) {
  pacha_kernel_sched_state sched;
  pacha_kernel_sched_decision decision;
  pacha_eevdf_runqueue scratch;
  pacha_eevdf_pick_result pick;
  const pacha_eevdf_entity *sender = NULL;
  const pacha_eevdf_entity *receiver = NULL;
  pacha_kernel_sched_empty_state(1, &sched);
  assert_valid(&sched);

  assert(pacha_kernel_sched_add_thread(
      &sched, 0, 10, 1, 1024, 4000000, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert(pacha_kernel_sched_add_thread(
      &sched, 0, 20, 1, 1024, 4000000, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);

  assert(pacha_kernel_sched_pick_cpu(&sched, 0, &decision, &pick, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);
  assert(decision.kind == PACHA_KERNEL_SCHED_DECISION_RUN_THREAD);
  assert(decision.thread_id == 10);
  assert(sched.cpus[0].current_thread_id == 10);
  assert(sched.runqueues[0].runnable_count == 1);

  assert(pacha_kernel_sched_handoff_to_thread_on_cpu(
      &sched, 0, 20, &decision, &scratch) == PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);
  assert(decision.kind == PACHA_KERNEL_SCHED_DECISION_RUN_THREAD);
  assert(decision.cpu_id == 0);
  assert(decision.thread_id == 20);
  assert(sched.cpus[0].has_current);
  assert(sched.cpus[0].current_thread_id == 20);
  assert(sched.cpus[0].current_generation == 1);
  assert(sched.runqueues[0].runnable_count == 1);

  sender = find_entity(&sched.runqueues[0], 10);
  receiver = find_entity(&sched.runqueues[0], 20);
  assert(sender != NULL);
  assert(receiver != NULL);
  assert(sender->state == PACHA_EEVDF_RUNNABLE);
  assert(receiver->state == PACHA_EEVDF_RUNNING);

  assert(pacha_kernel_sched_handoff_to_thread_on_cpu(
      &sched, 0, 10, &decision, &scratch) == PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);
  assert(sched.cpus[0].current_thread_id == 10);
}

static void test_handoff_rejects_bad_state(void) {
  pacha_kernel_sched_state sched;
  pacha_kernel_sched_decision decision;
  pacha_eevdf_runqueue scratch;
  pacha_eevdf_pick_result pick;
  pacha_kernel_sched_empty_state(1, &sched);
  assert_valid(&sched);

  assert(pacha_kernel_sched_handoff_to_thread_on_cpu(
      &sched, 0, 20, &decision, &scratch) == PACHA_KERNEL_SCHED_ERR_STATE);
  assert(pacha_kernel_sched_handoff_to_thread_on_cpu(
      &sched, 1, 20, &decision, &scratch) == PACHA_KERNEL_SCHED_ERR_INVALID);
  assert_valid(&sched);

  assert(pacha_kernel_sched_add_thread(
      &sched, 0, 10, 1, 1024, 4000000, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert(pacha_kernel_sched_pick_cpu(&sched, 0, &decision, &pick, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);

  assert(pacha_kernel_sched_handoff_to_thread_on_cpu(
      &sched, 0, 99, &decision, &scratch) == PACHA_KERNEL_SCHED_ERR_INVALID);
  assert(pacha_kernel_sched_handoff_to_thread_on_cpu(
      &sched, 0, 10, &decision, &scratch) == PACHA_KERNEL_SCHED_ERR_STATE);
  assert_valid(&sched);
}

static void test_add_rejects_global_duplicate_thread_id(void) {
  pacha_kernel_sched_state sched;
  pacha_kernel_sched_decision decision;
  pacha_eevdf_runqueue scratch;
  pacha_kernel_sched_empty_state(2, &sched);
  assert_valid(&sched);

  assert(pacha_kernel_sched_add_thread(
      &sched, 0, 9, 1, 1024, 4000000, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);
  assert(pacha_kernel_sched_add_thread(
      &sched, 1, 9, 2, 1024, 4000000, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_ERR_INVALID);
  assert_valid(&sched);
  assert(sched.runqueues[0].entity_count == 1);
  assert(sched.runqueues[1].entity_count == 0);
}

static void test_add_reuses_exited_thread_slot(void) {
  pacha_kernel_sched_state sched;
  pacha_kernel_sched_decision decision;
  pacha_eevdf_runqueue scratch;
  pacha_kernel_sched_empty_state(1, &sched);
  assert_valid(&sched);

  assert(pacha_kernel_sched_add_thread(
      &sched, 0, 12, 1, 1024, 4000000, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);
  assert(pacha_kernel_sched_exit_thread(&sched, 12, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);
  assert(pacha_kernel_sched_add_thread(
      &sched, 0, 12, 2, 1024, 4000000, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);
  assert(sched.runqueues[0].entity_count == 1);
  assert(sched.runqueues[0].runnable_count == 1);
  assert(sched.runqueues[0].entities[0].thread_id == 12);
  assert(sched.runqueues[0].entities[0].generation == 2);
}

static void test_block_wake_exit_by_thread_lookup(void) {
  pacha_kernel_sched_state sched;
  pacha_kernel_sched_decision decision;
  pacha_eevdf_runqueue scratch;
  pacha_eevdf_pick_result pick;
  pacha_kernel_sched_empty_state(2, &sched);
  assert_valid(&sched);

  assert(pacha_kernel_sched_add_thread(
      &sched, 1, 30, 1, 1024, 4000000, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);
  assert(pacha_kernel_sched_wake_thread(&sched, 99, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_ERR_INVALID);
  assert_valid(&sched);
  assert(pacha_kernel_sched_pick_cpu(&sched, 1, &decision, &pick, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);
  assert(sched.cpus[1].has_current);

  assert(pacha_kernel_sched_block_thread(&sched, 30, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);
  assert(!sched.cpus[1].has_current);
  assert(sched.runqueues[1].runnable_count == 0);

  assert(pacha_kernel_sched_wake_thread(&sched, 30, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);
  assert(sched.runqueues[1].runnable_count == 1);

  assert(pacha_kernel_sched_exit_thread(&sched, 30, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);
  assert(sched.runqueues[1].runnable_count == 0);
  assert(pacha_kernel_sched_block_thread(&sched, 99, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_ERR_INVALID);
  assert(pacha_kernel_sched_exit_thread(&sched, 99, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_ERR_INVALID);
  assert_valid(&sched);
}

static void test_migrate_runnable_between_cpus(void) {
  pacha_kernel_sched_state sched;
  pacha_kernel_sched_decision decision;
  pacha_eevdf_runqueue scratch;
  pacha_eevdf_runqueue migrate_scratch;
  pacha_eevdf_pick_result pick;
  const pacha_eevdf_entity *moved = NULL;
  int64_t dst_floor = 0;
  pacha_kernel_sched_empty_state(2, &sched);
  assert_valid(&sched);

  assert(pacha_kernel_sched_add_thread(
      &sched, 0, 11, 1, 1024, 4000000, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);
  assert(pacha_kernel_sched_add_thread(
      &sched, 1, 22, 2, 1024, 4000000, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);
  assert(pacha_kernel_sched_pick_cpu(&sched, 1, &decision, &pick, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);
  assert(decision.thread_id == 22);
  assert(pacha_kernel_sched_on_timer(&sched, 1, 4096, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);
  assert(pacha_kernel_sched_finish_current(&sched, 1, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);
  dst_floor = sched.runqueues[1].min_vruntime;
  assert(dst_floor > 0);

  assert(pacha_kernel_sched_migrate_runnable(
      &sched, 0, 0, 11, &decision, &scratch, &migrate_scratch) ==
      PACHA_KERNEL_SCHED_ERR_INVALID);
  assert_valid(&sched);
  assert(pacha_kernel_sched_migrate_runnable(
      &sched, 0, 1, 11, &decision, &scratch, &migrate_scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);
  assert(sched.runqueues[0].entity_count == 0);
  assert(sched.runqueues[1].entity_count == 2);
  assert(sched.runqueues[1].runnable_count == 2);
  moved = find_entity(&sched.runqueues[1], 11);
  assert(moved != NULL);
  assert(moved->generation == 1);
  assert(moved->state == PACHA_EEVDF_RUNNABLE);
  assert(moved->service_ns == 0);
  assert(moved->vruntime == dst_floor);
  assert(moved->eligible_time == dst_floor);

  assert(pacha_kernel_sched_pick_cpu(&sched, 1, &decision, &pick, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);
  assert(decision.kind == PACHA_KERNEL_SCHED_DECISION_RUN_THREAD);
  assert(decision.thread_id == 11);
  assert(pacha_kernel_sched_migrate_runnable(
      &sched, 1, 0, 11, &decision, &scratch, &migrate_scratch) ==
      PACHA_KERNEL_SCHED_ERR_STATE);
  assert_valid(&sched);
}

static void test_migrate_preserves_unrelated_cpu_current(void) {
  pacha_kernel_sched_state sched;
  pacha_kernel_sched_decision decision;
  pacha_eevdf_runqueue scratch;
  pacha_eevdf_runqueue migrate_scratch;
  pacha_eevdf_pick_result pick;
  const pacha_eevdf_entity *src_current = NULL;
  const pacha_eevdf_entity *dst_current = NULL;
  const pacha_eevdf_entity *moved = NULL;
  pacha_kernel_sched_empty_state(2, &sched);
  assert_valid(&sched);

  assert(pacha_kernel_sched_add_thread(
      &sched, 0, 33, 1, 1024, 4000000, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert(pacha_kernel_sched_add_thread(
      &sched, 1, 22, 1, 1024, 4000000, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);

  assert(pacha_kernel_sched_pick_cpu(&sched, 0, &decision, &pick, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert(decision.kind == PACHA_KERNEL_SCHED_DECISION_RUN_THREAD);
  assert(decision.thread_id == 33);
  assert(pacha_kernel_sched_add_thread(
      &sched, 0, 11, 1, 1024, 4000000, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert(pacha_kernel_sched_pick_cpu(&sched, 1, &decision, &pick, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert(decision.kind == PACHA_KERNEL_SCHED_DECISION_RUN_THREAD);
  assert(decision.thread_id == 22);
  assert_valid(&sched);

  assert(sched.cpus[0].has_current);
  assert(sched.cpus[0].current_thread_id == 33);
  assert(sched.cpus[1].has_current);
  assert(sched.cpus[1].current_thread_id == 22);
  assert(pacha_kernel_sched_migrate_runnable(
      &sched, 0, 1, 11, &decision, &scratch, &migrate_scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);

  assert(sched.cpus[0].has_current);
  assert(sched.cpus[0].current_thread_id == 33);
  assert(sched.cpus[1].has_current);
  assert(sched.cpus[1].current_thread_id == 22);
  src_current = find_entity(&sched.runqueues[0], 33);
  dst_current = find_entity(&sched.runqueues[1], 22);
  moved = find_entity(&sched.runqueues[1], 11);
  assert(src_current != NULL);
  assert(src_current->state == PACHA_EEVDF_RUNNING);
  assert(dst_current != NULL);
  assert(dst_current->state == PACHA_EEVDF_RUNNING);
  assert(moved != NULL);
  assert(moved->state == PACHA_EEVDF_RUNNABLE);
}

static void test_no_current_and_invalid_cpu_branches(void) {
  pacha_kernel_sched_state sched;
  pacha_kernel_sched_decision decision;
  pacha_eevdf_runqueue scratch;
  pacha_eevdf_pick_result pick;
  pacha_kernel_sched_empty_state(1, &sched);
  assert_valid(&sched);

  assert(pacha_kernel_sched_finish_current(&sched, 0, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);
  assert(decision.kind == PACHA_KERNEL_SCHED_DECISION_NONE);

  assert(pacha_kernel_sched_on_timer(&sched, 0, 1000, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);
  assert(decision.kind == PACHA_KERNEL_SCHED_DECISION_NONE);

  assert(pacha_kernel_sched_request_activation(&sched, 1, &decision) ==
      PACHA_KERNEL_SCHED_ERR_INVALID);
  assert(pacha_kernel_sched_pick_cpu(&sched, 1, &decision, &pick, &scratch) ==
      PACHA_KERNEL_SCHED_ERR_INVALID);
  assert(pacha_kernel_sched_finish_current(&sched, 1, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_ERR_INVALID);
  assert(pacha_kernel_sched_on_timer(&sched, 1, 1000, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_ERR_INVALID);
  assert(pacha_kernel_sched_claim_activation(
      &sched, 1, &decision, &pick, &scratch) == PACHA_KERNEL_SCHED_ERR_INVALID);
  assert_valid(&sched);
}

static void test_validator_rejects_corrupt_state(void) {
  pacha_kernel_sched_state sched;
  pacha_kernel_sched_decision decision;
  pacha_eevdf_runqueue scratch;
  pacha_eevdf_pick_result pick;
  pacha_kernel_sched_empty_state(2, &sched);

  assert(pacha_kernel_sched_add_thread(
      &sched, 0, 40, 1, 1024, 4000000, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);

  sched.runqueues[1].entities[0] = sched.runqueues[0].entities[0];
  sched.runqueues[1].entity_count = 1;
  sched.runqueues[1].runnable_count = 1;
  assert(pacha_kernel_sched_validate(&sched) == PACHA_KERNEL_SCHED_ERR_STATE);

  pacha_kernel_sched_empty_state(1, &sched);
  assert(pacha_kernel_sched_add_thread(
      &sched, 0, 41, 1, 1024, 4000000, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert(pacha_kernel_sched_pick_cpu(&sched, 0, &decision, &pick, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);

  sched.cpus[0].current_thread_id = 99;
  assert(pacha_kernel_sched_validate(&sched) == PACHA_KERNEL_SCHED_ERR_STATE);
}

static void test_stale_current_generation_is_cleared(void) {
  pacha_kernel_sched_state sched;
  pacha_kernel_sched_decision decision;
  pacha_eevdf_runqueue scratch;
  pacha_eevdf_pick_result pick;
  const pacha_eevdf_entity *entity = NULL;
  pacha_kernel_sched_empty_state(1, &sched);
  assert_valid(&sched);

  assert(pacha_kernel_sched_add_thread(
      &sched, 0, 55, 7, 1024, 4000000, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);
  assert(pacha_kernel_sched_pick_cpu(&sched, 0, &decision, &pick, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);
  assert(decision.thread_id == 55);
  assert(decision.generation == 7);

  assert(pacha_kernel_sched_exit_thread(&sched, 55, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert_valid(&sched);
  sched.cpus[0].has_current = 1;
  sched.cpus[0].current_thread_id = 55;
  sched.cpus[0].current_generation = 7;

  assert(pacha_kernel_sched_on_timer(&sched, 0, 1000, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert(!sched.cpus[0].has_current);
  entity = find_entity(&sched.runqueues[0], 55);
  assert(entity == NULL);
  assert_valid(&sched);

  sched.cpus[0].has_current = 1;
  sched.cpus[0].current_thread_id = 55;
  sched.cpus[0].current_generation = 7;
  assert(pacha_kernel_sched_finish_current(&sched, 0, &decision, &scratch) ==
      PACHA_KERNEL_SCHED_OK);
  assert(!sched.cpus[0].has_current);
  assert_valid(&sched);
}

int main(void) {
  test_empty_state_clamps_cpus();
  test_per_cpu_pick_and_finish();
  test_activation_claim_clears_pending();
  test_handoff_to_woken_thread_on_cpu();
  test_handoff_rejects_bad_state();
  test_add_rejects_global_duplicate_thread_id();
  test_add_reuses_exited_thread_slot();
  test_block_wake_exit_by_thread_lookup();
  test_migrate_runnable_between_cpus();
  test_migrate_preserves_unrelated_cpu_current();
  test_no_current_and_invalid_cpu_branches();
  test_validator_rejects_corrupt_state();
  test_stale_current_generation_is_cleared();
  return 0;
}
