From Stdlib Require Import Lists.List ZArith.ZArith.
From Pacha.Scheduling Require Import
  EevdfModel
  EevdfTransitions
  SchedRuntimeModel.

Open Scope Z_scope.

Definition tv_sched_pick_start : sched_state :=
  sched_empty_state 2.

Definition tv_sched_pick_add_a : sched_state :=
  fst (sched_add_thread tv_sched_pick_start 20 1 1024 4000000).

Definition tv_sched_pick_add_b : sched_state :=
  fst (sched_add_thread tv_sched_pick_add_a 10 1 1024 4000000).

Definition tv_sched_pick_result : sched_state * sched_result :=
  sched_pick tv_sched_pick_add_b 0.

Example tv_sched_pick_runs_lowest_thread_id :
  sd_thread_id (sr_decision (snd tv_sched_pick_result)) = 10.
Proof.
  vm_compute.
  reflexivity.
Qed.

Example tv_sched_pick_marks_cpu_current :
  sc_current_thread_id
    (nth 0 (ss_cpus (fst tv_sched_pick_result)) sched_empty_cpu) =
  10.
Proof.
  vm_compute.
  reflexivity.
Qed.

Example tv_sched_pick_marks_cpu_generation :
  sc_current_generation
    (nth 0 (ss_cpus (fst tv_sched_pick_result)) sched_empty_cpu) =
  1.
Proof.
  vm_compute.
  reflexivity.
Qed.

Example tv_sched_pick_reduces_runnable_count :
  er_runnable_count (ss_runqueue (fst tv_sched_pick_result)) = 1%nat.
Proof.
  vm_compute.
  reflexivity.
Qed.

Definition tv_sched_second_pick_result : sched_state * sched_result :=
  sched_pick (fst tv_sched_pick_result) 0.

Example tv_sched_second_pick_fails_while_cpu_current :
  sr_rc (snd tv_sched_second_pick_result) = SchedErrState.
Proof.
  vm_compute.
  reflexivity.
Qed.

Definition tv_sched_tick_result : sched_state * sched_result :=
  sched_on_timer (fst tv_sched_pick_result) 0 1000.

Example tv_sched_timer_charges_current :
  ee_service_ns
    (nth 1 (er_entities (ss_runqueue (fst tv_sched_tick_result)))
      eevdf_empty_entity) =
  1000.
Proof.
  vm_compute.
  reflexivity.
Qed.

Definition tv_sched_finish_result : sched_state * sched_result :=
  sched_finish_current (fst tv_sched_tick_result) 0.

Example tv_sched_finish_clears_cpu :
  sc_has_current
    (nth 0 (ss_cpus (fst tv_sched_finish_result)) sched_empty_cpu) =
  false.
Proof.
  vm_compute.
  reflexivity.
Qed.

Example tv_sched_finish_requeues_current :
  er_runnable_count (ss_runqueue (fst tv_sched_finish_result)) = 2%nat.
Proof.
  vm_compute.
  reflexivity.
Qed.

Definition tv_sched_idle_state : sched_state :=
  sched_empty_state 1.

Example tv_sched_invalid_cpu :
  sr_rc (snd (sched_pick tv_sched_idle_state 1)) = SchedErrInvalid.
Proof.
  vm_compute.
  reflexivity.
Qed.

Example tv_sched_idle_decision :
  sd_kind (sr_decision (snd (sched_pick tv_sched_idle_state 0))) =
  SchedDecisionIdle.
Proof.
  vm_compute.
  reflexivity.
Qed.

Definition tv_sched_lifecycle_start : sched_state :=
  fst (sched_add_thread (sched_empty_state 1) 1 7 1024 4000000).

Definition tv_sched_lifecycle_pick : sched_state * sched_result :=
  sched_pick tv_sched_lifecycle_start 0.

Definition tv_sched_lifecycle_block : sched_state * sched_result :=
  sched_block_thread (fst tv_sched_lifecycle_pick) 1.

Example tv_sched_block_clears_current :
  sc_has_current
    (nth 0 (ss_cpus (fst tv_sched_lifecycle_block)) sched_empty_cpu) =
  false.
Proof.
  vm_compute.
  reflexivity.
Qed.

Example tv_sched_block_removes_runnable :
  er_runnable_count (ss_runqueue (fst tv_sched_lifecycle_block)) = 0%nat.
Proof.
  vm_compute.
  reflexivity.
Qed.

Definition tv_sched_lifecycle_wake : sched_state * sched_result :=
  sched_wake_thread (fst tv_sched_lifecycle_block) 1.

Example tv_sched_wake_restores_runnable :
  er_runnable_count (ss_runqueue (fst tv_sched_lifecycle_wake)) = 1%nat.
Proof.
  vm_compute.
  reflexivity.
Qed.

Definition tv_sched_lifecycle_pick_again : sched_state * sched_result :=
  sched_pick (fst tv_sched_lifecycle_wake) 0.

Definition tv_sched_lifecycle_exit : sched_state * sched_result :=
  sched_exit_thread (fst tv_sched_lifecycle_pick_again) 1.

Example tv_sched_exit_clears_current :
  sc_has_current
    (nth 0 (ss_cpus (fst tv_sched_lifecycle_exit)) sched_empty_cpu) =
  false.
Proof.
  vm_compute.
  reflexivity.
Qed.

Example tv_sched_exit_removes_runnable :
  er_runnable_count (ss_runqueue (fst tv_sched_lifecycle_exit)) = 0%nat.
Proof.
  vm_compute.
  reflexivity.
Qed.
