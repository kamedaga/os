#include "pacha_kernel_sched.h"

#include <assert.h>
#include <stdint.h>

static uint64_t rng_state = 0x706163686b736368ULL;

static uint64_t next_rand(void) {
  rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
  return rng_state;
}

static void assert_valid(const pacha_kernel_sched_state *sched) {
  assert(pacha_kernel_sched_validate(sched) == PACHA_KERNEL_SCHED_OK);
}

static size_t choose_cpu(const pacha_kernel_sched_state *sched) {
  return (size_t)(next_rand() % sched->cpu_count);
}

static int choose_active_thread(
    const pacha_kernel_sched_state *sched,
    int64_t *thread_id_out) {
  size_t total = 0;
  for (size_t cpu_id = 0; cpu_id < sched->cpu_count; ++cpu_id) {
    total += sched->runqueues[cpu_id].entity_count;
  }
  if (total == 0) {
    return 0;
  }

  size_t selected = (size_t)(next_rand() % total);
  for (size_t cpu_id = 0; cpu_id < sched->cpu_count; ++cpu_id) {
    const pacha_eevdf_runqueue *rq = &sched->runqueues[cpu_id];
    if (selected < rq->entity_count) {
      *thread_id_out = rq->entities[selected].thread_id;
      return 1;
    }
    selected -= rq->entity_count;
  }
  return 0;
}

static int choose_runnable_thread(
    const pacha_kernel_sched_state *sched,
    size_t *cpu_out,
    int64_t *thread_id_out) {
  size_t total = 0;
  for (size_t cpu_id = 0; cpu_id < sched->cpu_count; ++cpu_id) {
    const pacha_eevdf_runqueue *rq = &sched->runqueues[cpu_id];
    for (size_t index = 0; index < rq->entity_count; ++index) {
      if (rq->entities[index].state == PACHA_EEVDF_RUNNABLE) {
        ++total;
      }
    }
  }
  if (total == 0) {
    return 0;
  }

  size_t selected = (size_t)(next_rand() % total);
  for (size_t cpu_id = 0; cpu_id < sched->cpu_count; ++cpu_id) {
    const pacha_eevdf_runqueue *rq = &sched->runqueues[cpu_id];
    for (size_t index = 0; index < rq->entity_count; ++index) {
      if (rq->entities[index].state != PACHA_EEVDF_RUNNABLE) {
        continue;
      }
      if (selected == 0) {
        *cpu_out = cpu_id;
        *thread_id_out = rq->entities[index].thread_id;
        return 1;
      }
      --selected;
    }
  }
  return 0;
}

static void run_property_sequence(void) {
  pacha_kernel_sched_state sched;
  pacha_kernel_sched_decision decision;
  pacha_eevdf_runqueue scratch_a;
  pacha_eevdf_runqueue scratch_b;
  pacha_eevdf_pick_result pick_scratch;
  int64_t next_thread_id = 1;
  pacha_kernel_sched_empty_state(4, &sched);
  assert_valid(&sched);

  for (size_t step = 0; step < 3000; ++step) {
    uint64_t op = next_rand() % 10;
    size_t cpu_id = choose_cpu(&sched);
    int64_t thread_id = PACHA_EEVDF_NO_THREAD_ID;

    switch (op) {
    case 0:
      if (next_thread_id < 180) {
        (void)pacha_kernel_sched_add_thread(
            &sched,
            cpu_id,
            next_thread_id++,
            (int64_t)step + 1,
            1 + (int64_t)(next_rand() % 4096),
            1 + (int64_t)(next_rand() % 8000000),
            &decision,
            &scratch_a);
      }
      break;
    case 1:
      (void)pacha_kernel_sched_pick_cpu(
          &sched,
          cpu_id,
          &decision,
          &pick_scratch,
          &scratch_a);
      break;
    case 2:
      (void)pacha_kernel_sched_finish_current(
          &sched,
          cpu_id,
          &decision,
          &scratch_a);
      break;
    case 3:
      (void)pacha_kernel_sched_on_timer(
          &sched,
          cpu_id,
          (int64_t)(next_rand() % 100000),
          &decision,
          &scratch_a);
      break;
    case 4:
      if (choose_active_thread(&sched, &thread_id)) {
        (void)pacha_kernel_sched_block_thread(
            &sched,
            thread_id,
            &decision,
            &scratch_a);
      }
      break;
    case 5:
      if (choose_active_thread(&sched, &thread_id)) {
        (void)pacha_kernel_sched_wake_thread(
            &sched,
            thread_id,
            &decision,
            &scratch_a);
      }
      break;
    case 6:
      if (choose_active_thread(&sched, &thread_id)) {
        (void)pacha_kernel_sched_exit_thread(
            &sched,
            thread_id,
            &decision,
            &scratch_a);
      }
      break;
    case 7: {
      size_t src_cpu = 0;
      if (choose_runnable_thread(&sched, &src_cpu, &thread_id)) {
        size_t dst_cpu = choose_cpu(&sched);
        if (dst_cpu == src_cpu) {
          dst_cpu = (dst_cpu + 1) % sched.cpu_count;
        }
        (void)pacha_kernel_sched_migrate_runnable(
            &sched,
            src_cpu,
            dst_cpu,
            thread_id,
            &decision,
            &scratch_a,
            &scratch_b);
      }
      break;
    }
    case 8:
      (void)pacha_kernel_sched_request_activation(&sched, cpu_id, &decision);
      break;
    default:
      (void)pacha_kernel_sched_claim_activation(
          &sched,
          cpu_id,
          &decision,
          &pick_scratch,
          &scratch_a);
      break;
    }

    assert_valid(&sched);
  }
}

int main(void) {
  run_property_sequence();
  return 0;
}
