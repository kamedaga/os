#include "pacha_sched.h"

#include <assert.h>

static void test_pick_and_finish_current(void) {
  pacha_sched_state sched = pacha_sched_empty_state(2);

  pacha_sched_result add_a =
      pacha_sched_add_thread(&sched, 20, 1, 1024, 4000000);
  assert(add_a.rc == PACHA_SCHED_OK);
  pacha_sched_result add_b =
      pacha_sched_add_thread(&sched, 10, 1, 1024, 4000000);
  assert(add_b.rc == PACHA_SCHED_OK);

  pacha_sched_result pick = pacha_sched_pick(&sched, 0);
  assert(pick.rc == PACHA_SCHED_OK);
  assert(pick.decision.kind == PACHA_SCHED_DECISION_RUN_THREAD);
  assert(pick.decision.cpu_id == 0);
  assert(pick.decision.thread_id == 10);
  assert(pick.decision.generation == 1);
  assert(sched.cpus[0].has_current);
  assert(sched.cpus[0].current_thread_id == 10);
  assert(sched.runqueue.runnable_count == 1);

  pacha_sched_result second_pick = pacha_sched_pick(&sched, 0);
  assert(second_pick.rc == PACHA_SCHED_ERR_STATE);

  pacha_sched_result tick = pacha_sched_on_timer(&sched, 0, 1000);
  assert(tick.rc == PACHA_SCHED_OK);

  pacha_sched_result finish = pacha_sched_finish_current(&sched, 0);
  assert(finish.rc == PACHA_SCHED_OK);
  assert(!sched.cpus[0].has_current);
  assert(sched.runqueue.runnable_count == 2);
}

static void test_idle_and_invalid_cpu(void) {
  pacha_sched_state sched = pacha_sched_empty_state(1);

  pacha_sched_result invalid_pick = pacha_sched_pick(&sched, 1);
  assert(invalid_pick.rc == PACHA_SCHED_ERR_INVALID);

  pacha_sched_result idle = pacha_sched_pick(&sched, 0);
  assert(idle.rc == PACHA_SCHED_OK);
  assert(idle.decision.kind == PACHA_SCHED_DECISION_IDLE);
  assert(idle.decision.cpu_id == 0);
  assert(!sched.cpus[0].has_current);
}

static void test_block_wake_exit_lifecycle(void) {
  pacha_sched_state sched = pacha_sched_empty_state(1);
  assert(pacha_sched_add_thread(&sched, 1, 7, 1024, 4000000).rc ==
      PACHA_SCHED_OK);

  pacha_sched_result pick = pacha_sched_pick(&sched, 0);
  assert(pick.rc == PACHA_SCHED_OK);
  assert(pick.decision.thread_id == 1);

  pacha_sched_result block = pacha_sched_block_thread(&sched, 1);
  assert(block.rc == PACHA_SCHED_OK);
  assert(!sched.cpus[0].has_current);
  assert(sched.runqueue.runnable_count == 0);

  pacha_sched_result idle = pacha_sched_pick(&sched, 0);
  assert(idle.rc == PACHA_SCHED_OK);
  assert(idle.decision.kind == PACHA_SCHED_DECISION_IDLE);

  pacha_sched_result wake = pacha_sched_wake_thread(&sched, 1);
  assert(wake.rc == PACHA_SCHED_OK);
  assert(sched.runqueue.runnable_count == 1);

  pacha_sched_result pick_again = pacha_sched_pick(&sched, 0);
  assert(pick_again.rc == PACHA_SCHED_OK);
  assert(pick_again.decision.kind == PACHA_SCHED_DECISION_RUN_THREAD);
  assert(pick_again.decision.thread_id == 1);

  pacha_sched_result exit = pacha_sched_exit_thread(&sched, 1);
  assert(exit.rc == PACHA_SCHED_OK);
  assert(!sched.cpus[0].has_current);
  assert(sched.runqueue.runnable_count == 0);
}

int main(void) {
  test_pick_and_finish_current();
  test_idle_and_invalid_cpu();
  test_block_wake_exit_lifecycle();
  return 0;
}
