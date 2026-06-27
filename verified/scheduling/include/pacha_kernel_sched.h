#ifndef PACHA_KERNEL_SCHED_H
#define PACHA_KERNEL_SCHED_H

#include "pacha_eevdf.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PACHA_KERNEL_SCHED_MAX_CPUS 256u
#define PACHA_KERNEL_SCHED_NO_CPU PACHA_KERNEL_SCHED_MAX_CPUS

typedef enum pacha_kernel_sched_rc {
  PACHA_KERNEL_SCHED_OK = 0,
  PACHA_KERNEL_SCHED_ERR_INVALID = 1,
  PACHA_KERNEL_SCHED_ERR_FULL = 2,
  PACHA_KERNEL_SCHED_ERR_OVERFLOW = 3,
  PACHA_KERNEL_SCHED_ERR_STATE = 4,
} pacha_kernel_sched_rc;

typedef enum pacha_kernel_sched_decision_kind {
  PACHA_KERNEL_SCHED_DECISION_NONE = 0,
  PACHA_KERNEL_SCHED_DECISION_RUN_THREAD = 1,
  PACHA_KERNEL_SCHED_DECISION_IDLE = 2,
} pacha_kernel_sched_decision_kind;

typedef struct pacha_kernel_sched_decision {
  pacha_kernel_sched_decision_kind kind;
  size_t cpu_id;
  int64_t thread_id;
  int64_t generation;
} pacha_kernel_sched_decision;

typedef struct pacha_kernel_sched_cpu {
  int has_current;
  int64_t current_thread_id;
  int64_t current_generation;
  int activation_pending;
} pacha_kernel_sched_cpu;

typedef struct pacha_kernel_sched_state {
  pacha_eevdf_runqueue runqueues[PACHA_KERNEL_SCHED_MAX_CPUS];
  pacha_kernel_sched_cpu cpus[PACHA_KERNEL_SCHED_MAX_CPUS];
  size_t cpu_count;
  size_t balance_cursor;
} pacha_kernel_sched_state;

void pacha_kernel_sched_no_decision(pacha_kernel_sched_decision *out);
void pacha_kernel_sched_empty_state(
    size_t cpu_count,
    pacha_kernel_sched_state *out);

pacha_kernel_sched_rc pacha_kernel_sched_add_thread(
    pacha_kernel_sched_state *sched,
    size_t cpu_id,
    int64_t thread_id,
    int64_t generation,
    int64_t weight,
    int64_t slice_ns,
    pacha_kernel_sched_decision *decision_out,
    pacha_eevdf_runqueue *scratch);
pacha_kernel_sched_rc pacha_kernel_sched_wake_thread(
    pacha_kernel_sched_state *sched,
    int64_t thread_id,
    pacha_kernel_sched_decision *decision_out,
    pacha_eevdf_runqueue *scratch);
pacha_kernel_sched_rc pacha_kernel_sched_wake_thread_on_cpu(
    pacha_kernel_sched_state *sched,
    size_t cpu_id,
    int64_t thread_id,
    pacha_kernel_sched_decision *decision_out);
pacha_kernel_sched_rc pacha_kernel_sched_block_thread(
    pacha_kernel_sched_state *sched,
    int64_t thread_id,
    pacha_kernel_sched_decision *decision_out,
    pacha_eevdf_runqueue *scratch);
pacha_kernel_sched_rc pacha_kernel_sched_block_thread_on_cpu(
    pacha_kernel_sched_state *sched,
    size_t cpu_id,
    int64_t thread_id,
    pacha_kernel_sched_decision *decision_out);
pacha_kernel_sched_rc pacha_kernel_sched_exit_thread(
    pacha_kernel_sched_state *sched,
    int64_t thread_id,
    pacha_kernel_sched_decision *decision_out,
    pacha_eevdf_runqueue *scratch);
pacha_kernel_sched_rc pacha_kernel_sched_exit_thread_on_cpu(
    pacha_kernel_sched_state *sched,
    size_t cpu_id,
    int64_t thread_id,
    pacha_kernel_sched_decision *decision_out);
pacha_kernel_sched_rc pacha_kernel_sched_on_timer(
    pacha_kernel_sched_state *sched,
    size_t cpu_id,
    int64_t runtime_ns,
    pacha_kernel_sched_decision *decision_out,
    pacha_eevdf_runqueue *scratch);
pacha_kernel_sched_rc pacha_kernel_sched_pick_cpu(
    pacha_kernel_sched_state *sched,
    size_t cpu_id,
    pacha_kernel_sched_decision *decision_out,
    pacha_eevdf_pick_result *pick_scratch,
    pacha_eevdf_runqueue *scratch);
pacha_kernel_sched_rc pacha_kernel_sched_finish_current(
    pacha_kernel_sched_state *sched,
    size_t cpu_id,
    pacha_kernel_sched_decision *decision_out,
    pacha_eevdf_runqueue *scratch);
pacha_kernel_sched_rc pacha_kernel_sched_handoff_to_thread_on_cpu(
    pacha_kernel_sched_state *sched,
    size_t cpu_id,
    int64_t thread_id,
    pacha_kernel_sched_decision *decision_out,
    pacha_eevdf_runqueue *scratch);
pacha_kernel_sched_rc pacha_kernel_sched_request_activation(
    pacha_kernel_sched_state *sched,
    size_t cpu_id,
    pacha_kernel_sched_decision *decision_out);
pacha_kernel_sched_rc pacha_kernel_sched_claim_activation(
    pacha_kernel_sched_state *sched,
    size_t cpu_id,
    pacha_kernel_sched_decision *decision_out,
    pacha_eevdf_pick_result *pick_scratch,
    pacha_eevdf_runqueue *scratch);
pacha_kernel_sched_rc pacha_kernel_sched_migrate_runnable(
    pacha_kernel_sched_state *sched,
    size_t src_cpu,
    size_t dst_cpu,
    int64_t thread_id,
    pacha_kernel_sched_decision *decision_out,
    pacha_eevdf_runqueue *src_scratch,
    pacha_eevdf_runqueue *dst_scratch);

pacha_kernel_sched_rc pacha_kernel_sched_validate(
    const pacha_kernel_sched_state *sched);

#ifdef __cplusplus
}
#endif

#endif
