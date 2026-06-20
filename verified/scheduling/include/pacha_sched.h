#ifndef PACHA_SCHED_H
#define PACHA_SCHED_H

#include "pacha_eevdf.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PACHA_SCHED_MAX_CPUS 256u
#define PACHA_SCHED_NO_CPU SIZE_MAX

typedef enum pacha_sched_rc {
  PACHA_SCHED_OK = 0,
  PACHA_SCHED_ERR_INVALID = 1,
  PACHA_SCHED_ERR_FULL = 2,
  PACHA_SCHED_ERR_OVERFLOW = 3,
  PACHA_SCHED_ERR_STATE = 4,
} pacha_sched_rc;

typedef enum pacha_sched_decision_kind {
  PACHA_SCHED_DECISION_NONE = 0,
  PACHA_SCHED_DECISION_RUN_THREAD = 1,
  PACHA_SCHED_DECISION_IDLE = 2,
} pacha_sched_decision_kind;

typedef struct pacha_sched_decision {
  pacha_sched_decision_kind kind;
  size_t cpu_id;
  int64_t thread_id;
  int64_t generation;
} pacha_sched_decision;

typedef struct pacha_sched_result {
  pacha_sched_rc rc;
  pacha_sched_decision decision;
} pacha_sched_result;

typedef struct pacha_sched_cpu {
  int has_current;
  int64_t current_thread_id;
} pacha_sched_cpu;

typedef struct pacha_sched_state {
  pacha_eevdf_runqueue runqueue;
  pacha_sched_cpu cpus[PACHA_SCHED_MAX_CPUS];
  size_t cpu_count;
} pacha_sched_state;

pacha_sched_decision pacha_sched_no_decision(void);
pacha_sched_state pacha_sched_empty_state(size_t cpu_count);

pacha_sched_result pacha_sched_add_thread(
    pacha_sched_state *sched,
    int64_t thread_id,
    int64_t generation,
    int64_t weight,
    int64_t slice_ns);
pacha_sched_result pacha_sched_wake_thread(
    pacha_sched_state *sched,
    int64_t thread_id);
pacha_sched_result pacha_sched_block_thread(
    pacha_sched_state *sched,
    int64_t thread_id);
pacha_sched_result pacha_sched_exit_thread(
    pacha_sched_state *sched,
    int64_t thread_id);
pacha_sched_result pacha_sched_on_timer(
    pacha_sched_state *sched,
    size_t cpu_id,
    int64_t runtime_ns);
pacha_sched_result pacha_sched_pick(
    pacha_sched_state *sched,
    size_t cpu_id);
pacha_sched_result pacha_sched_finish_current(
    pacha_sched_state *sched,
    size_t cpu_id);

#ifdef __cplusplus
}
#endif

#endif
