From Stdlib Require Import Bool.Bool Lists.List ZArith.ZArith.
From Pacha.Scheduling Require Import
  EevdfModel
  EevdfInvariants
  KernelSchedModel.

Import ListNotations.
Open Scope Z_scope.

Definition kernel_sched_shape
    (sched : kernel_sched_state)
  : Prop :=
  (ks_cpu_count sched <= kernel_sched_max_cpus)%nat /\
  length (ks_runqueues sched) = kernel_sched_max_cpus /\
  length (ks_cpus sched) = kernel_sched_max_cpus.

Definition kernel_active_runqueue
    (sched : kernel_sched_state)
    (cpu_id : nat)
    (rq : eevdf_runqueue)
  : Prop :=
  (cpu_id < ks_cpu_count sched)%nat /\
  nth_error (ks_runqueues sched) cpu_id = Some rq.

Definition kernel_cpu_slot
    (sched : kernel_sched_state)
    (cpu_id : nat)
    (cpu : kernel_cpu)
  : Prop :=
  (cpu_id < ks_cpu_count sched)%nat /\
  nth_error (ks_cpus sched) cpu_id = Some cpu.

Definition kernel_runqueues_invariant
    (sched : kernel_sched_state)
  : Prop :=
  forall cpu_id rq,
    kernel_active_runqueue sched cpu_id rq ->
    eevdf_invariant rq.

Definition kernel_entity_on_cpu
    (sched : kernel_sched_state)
    (cpu_id : nat)
    (index : nat)
    (entity : eevdf_entity)
  : Prop :=
  exists rq,
    kernel_active_runqueue sched cpu_id rq /\
    eevdf_active_entity rq index entity /\
    is_active_state (ee_state entity) = true.

Definition kernel_global_thread_ids_unique
    (sched : kernel_sched_state)
  : Prop :=
  forall cpu_a index_a cpu_b index_b lhs rhs,
    kernel_entity_on_cpu sched cpu_a index_a lhs ->
    kernel_entity_on_cpu sched cpu_b index_b rhs ->
    ee_thread_id lhs <> no_thread_id ->
    ee_thread_id lhs = ee_thread_id rhs ->
    cpu_a = cpu_b /\ index_a = index_b.

Definition kernel_current_matches_local_running
    (sched : kernel_sched_state)
  : Prop :=
  forall cpu_id cpu,
    kernel_cpu_slot sched cpu_id cpu ->
    kc_has_current cpu = true ->
    exists rq index entity,
      kernel_active_runqueue sched cpu_id rq /\
      eevdf_active_entity rq index entity /\
      ee_state entity = ERunning /\
      ee_thread_id entity = kc_current_thread_id cpu /\
      ee_generation entity = kc_current_generation cpu.

Definition kernel_running_entity_has_current
    (sched : kernel_sched_state)
  : Prop :=
  forall cpu_id rq index entity,
    kernel_active_runqueue sched cpu_id rq ->
    eevdf_active_entity rq index entity ->
    ee_state entity = ERunning ->
    exists cpu,
      kernel_cpu_slot sched cpu_id cpu /\
      kc_has_current cpu = true /\
      kc_current_thread_id cpu = ee_thread_id entity /\
      kc_current_generation cpu = ee_generation entity.

Definition kernel_no_cross_cpu_current_duplicates
    (sched : kernel_sched_state)
  : Prop :=
  forall cpu_a cpu_b lhs rhs,
    kernel_cpu_slot sched cpu_a lhs ->
    kernel_cpu_slot sched cpu_b rhs ->
    kc_has_current lhs = true ->
    kc_has_current rhs = true ->
    kc_current_thread_id lhs <> no_thread_id ->
    kc_current_thread_id lhs = kc_current_thread_id rhs ->
    cpu_a = cpu_b.

Definition kernel_activation_targets_valid_cpus
    (sched : kernel_sched_state)
  : Prop :=
  forall cpu_id cpu,
    nth_error (ks_cpus sched) cpu_id = Some cpu ->
    kc_activation_pending cpu = true ->
    (cpu_id < ks_cpu_count sched)%nat.

Definition kernel_sched_invariant
    (sched : kernel_sched_state)
  : Prop :=
  kernel_sched_shape sched /\
  kernel_runqueues_invariant sched /\
  kernel_global_thread_ids_unique sched /\
  kernel_current_matches_local_running sched /\
  kernel_running_entity_has_current sched /\
  kernel_no_cross_cpu_current_duplicates sched /\
  kernel_activation_targets_valid_cpus sched.

Theorem kernel_sched_invariant_implies_shape :
  forall sched,
    kernel_sched_invariant sched ->
    kernel_sched_shape sched.
Proof.
  intros sched [Hshape _].
  exact Hshape.
Qed.

Theorem kernel_sched_invariant_implies_runqueues :
  forall sched,
    kernel_sched_invariant sched ->
    kernel_runqueues_invariant sched.
Proof.
  intros sched [_ [Hrqs _]].
  exact Hrqs.
Qed.

Theorem kernel_sched_invariant_implies_current_matches :
  forall sched,
    kernel_sched_invariant sched ->
    kernel_current_matches_local_running sched.
Proof.
  intros sched [_ [_ [_ [Hcurrent _]]]].
  exact Hcurrent.
Qed.
