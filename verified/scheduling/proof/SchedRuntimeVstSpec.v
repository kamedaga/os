From Stdlib Require Import Lists.List Lia ZArith.ZArith.
From VST Require Import floyd.proofauto.
From Pacha.Scheduling Require Import
  EevdfModel
  EevdfInvariants
  EevdfTransitions
  SchedRuntimeModel.

Import ListNotations.
Open Scope Z_scope.

Definition c_int64_min : Z := -9223372036854775808.
Definition c_int64_max : Z := 9223372036854775807.
Definition c_size_t_max : Z := 18446744073709551615.

Definition c_int64_value
    (value : Z)
  : Prop :=
  c_int64_min <= value <= c_int64_max.

Definition c_size_t_value
    (value : Z)
  : Prop :=
  0 <= value <= c_size_t_max.

Definition c_bool_int_value
    (value : Z)
  : Prop :=
  value = 0 \/ value = 1.

Definition sched_rc_c_value
    (rc : sched_rc)
  : Z :=
  match rc with
  | SchedOk => 0
  | SchedErrInvalid => 1
  | SchedErrFull => 2
  | SchedErrOverflow => 3
  | SchedErrState => 4
  end.

Definition eevdf_rc_c_value
    (rc : eevdf_rc)
  : Z :=
  match rc with
  | EevdfOk => 0
  | EevdfErrInvalid => 1
  | EevdfErrFull => 2
  | EevdfErrOverflow => 3
  | EevdfErrState => 4
  end.

Definition sched_decision_kind_c_value
    (kind : sched_decision_kind)
  : Z :=
  match kind with
  | SchedDecisionNone => 0
  | SchedDecisionRunThread => 1
  | SchedDecisionIdle => 2
  end.

Definition eevdf_state_c_value
    (state : eevdf_state)
  : Z :=
  match state with
  | EEmpty => 0
  | ERunnable => 1
  | ERunning => 2
  | EBlocked => 3
  | EExited => 4
  end.

Definition eevdf_entity_c_shape
    (entity : eevdf_entity)
  : Prop :=
  c_int64_value (ee_thread_id entity) /\
  c_int64_value (ee_generation entity) /\
  c_int64_value (ee_weight entity) /\
  c_int64_value (ee_slice_ns entity) /\
  c_int64_value (ee_service_ns entity) /\
  c_int64_value (ee_vruntime entity) /\
  c_int64_value (ee_eligible_time entity) /\
  c_int64_value (ee_deadline entity) /\
  c_int64_value (eevdf_state_c_value (ee_state entity)).

Definition eevdf_runqueue_c_shape
    (rq : eevdf_runqueue)
  : Prop :=
  c_size_t_value (Z.of_nat (er_entity_count rq)) /\
  c_size_t_value (Z.of_nat (er_runnable_count rq)) /\
  c_int64_value (er_virtual_time rq) /\
  c_int64_value (er_min_vruntime rq) /\
  length (er_entities rq) = eevdf_max_entities /\
  Forall eevdf_entity_c_shape (er_entities rq).

Definition sched_cpu_c_shape
    (cpu : sched_cpu)
  : Prop :=
  c_bool_int_value (if sc_has_current cpu then 1 else 0) /\
  c_int64_value (sc_current_thread_id cpu) /\
  c_int64_value (sc_current_generation cpu).

Definition sched_decision_c_shape
    (decision : sched_decision)
  : Prop :=
  c_int64_value (sched_decision_kind_c_value (sd_kind decision)) /\
  c_size_t_value (Z.of_nat (sd_cpu_id decision)) /\
  c_int64_value (sd_thread_id decision) /\
  c_int64_value (sd_generation decision).

Definition sched_result_c_shape
    (result : sched_result)
  : Prop :=
  c_int64_value (sched_rc_c_value (sr_rc result)) /\
  sched_decision_c_shape (sr_decision result).

Definition sched_state_c_shape
    (sched : sched_state)
  : Prop :=
  eevdf_runqueue_c_shape (ss_runqueue sched) /\
  length (ss_cpus sched) = sched_max_cpus /\
  Forall sched_cpu_c_shape (ss_cpus sched) /\
  c_size_t_value (Z.of_nat (ss_cpu_count sched)) /\
  (ss_cpu_count sched <= sched_max_cpus)%nat.

Definition eevdf_result_c_shape
    (result : eevdf_result)
  : Prop :=
  c_int64_value (eevdf_rc_c_value (eevdf_result_rc result)) /\
  eevdf_runqueue_c_shape (eevdf_result_rq result).

Definition eevdf_add_vst_pre
    (rq : eevdf_runqueue)
    (thread_id generation weight slice_ns : Z)
  : Prop :=
  eevdf_runqueue_c_shape rq /\
  c_int64_value thread_id /\
  c_int64_value generation /\
  c_int64_value weight /\
  c_int64_value slice_ns.

Definition eevdf_unary_thread_vst_pre
    (rq : eevdf_runqueue)
    (thread_id : Z)
  : Prop :=
  eevdf_runqueue_c_shape rq /\
  c_int64_value thread_id.

Definition eevdf_charge_vst_pre
    (rq : eevdf_runqueue)
    (thread_id runtime_ns : Z)
  : Prop :=
  eevdf_runqueue_c_shape rq /\
  c_int64_value thread_id /\
  c_int64_value runtime_ns.

Definition eevdf_add_vst_post
    (before after : eevdf_runqueue)
    (rc : eevdf_rc)
    (thread_id generation weight slice_ns : Z)
  : Prop :=
  let result := eevdf_add before thread_id generation weight slice_ns in
  after = eevdf_result_rq result /\
  rc = eevdf_result_rc result /\
  eevdf_result_c_shape result.

Definition eevdf_wake_vst_post
    (before after : eevdf_runqueue)
    (rc : eevdf_rc)
    (thread_id : Z)
  : Prop :=
  let result := eevdf_wake before thread_id in
  after = eevdf_result_rq result /\
  rc = eevdf_result_rc result /\
  eevdf_result_c_shape result.

Definition eevdf_block_vst_post
    (before after : eevdf_runqueue)
    (rc : eevdf_rc)
    (thread_id : Z)
  : Prop :=
  let result := eevdf_block before thread_id in
  after = eevdf_result_rq result /\
  rc = eevdf_result_rc result /\
  eevdf_result_c_shape result.

Definition eevdf_exit_vst_post
    (before after : eevdf_runqueue)
    (rc : eevdf_rc)
    (thread_id : Z)
  : Prop :=
  let result := eevdf_exit before thread_id in
  after = eevdf_result_rq result /\
  rc = eevdf_result_rc result /\
  eevdf_result_c_shape result.

Definition eevdf_charge_vst_post
    (before after : eevdf_runqueue)
    (rc : eevdf_rc)
    (thread_id runtime_ns : Z)
  : Prop :=
  let result := eevdf_charge before thread_id runtime_ns in
  after = eevdf_result_rq result /\
  rc = eevdf_result_rc result /\
  eevdf_result_c_shape result.

Definition eevdf_mark_running_vst_post
    (before after : eevdf_runqueue)
    (rc : eevdf_rc)
    (thread_id : Z)
  : Prop :=
  let result := eevdf_mark_running before thread_id in
  after = eevdf_result_rq result /\
  rc = eevdf_result_rc result /\
  eevdf_result_c_shape result.

Definition eevdf_requeue_running_vst_post
    (before after : eevdf_runqueue)
    (rc : eevdf_rc)
    (thread_id : Z)
  : Prop :=
  let result := eevdf_requeue_running before thread_id in
  after = eevdf_result_rq result /\
  rc = eevdf_result_rc result /\
  eevdf_result_c_shape result.

Parameter pacha_eevdf_runqueue_rep : eevdf_runqueue -> val -> mpred.
Parameter pacha_sched_state_rep : sched_state -> val -> mpred.
Parameter pacha_sched_decision_rep : sched_decision -> val -> mpred.

Definition sched_add_thread_vst_pre
    (sched : sched_state)
    (thread_id generation weight slice_ns : Z)
  : Prop :=
  sched_state_c_shape sched /\
  c_int64_value thread_id /\
  c_int64_value generation /\
  c_int64_value weight /\
  c_int64_value slice_ns.

Definition sched_unary_thread_vst_pre
    (sched : sched_state)
    (thread_id : Z)
  : Prop :=
  sched_state_c_shape sched /\
  c_int64_value thread_id.

Definition sched_cpu_vst_pre
    (sched : sched_state)
    (cpu_id : nat)
  : Prop :=
  sched_state_c_shape sched /\
  c_size_t_value (Z.of_nat cpu_id).

Definition sched_timer_vst_pre
    (sched : sched_state)
    (cpu_id : nat)
    (runtime_ns : Z)
  : Prop :=
  sched_cpu_vst_pre sched cpu_id /\
  c_int64_value runtime_ns.

Definition sched_add_thread_vst_post
    (before after : sched_state)
    (rc : sched_rc)
    (decision : sched_decision)
    (thread_id generation weight slice_ns : Z)
  : Prop :=
  exists result,
    sched_add_thread before thread_id generation weight slice_ns =
      (after, result) /\
    rc = sr_rc result /\
    decision = sr_decision result /\
    sched_result_c_shape result.

Definition sched_wake_thread_vst_post
    (before after : sched_state)
    (rc : sched_rc)
    (decision : sched_decision)
    (thread_id : Z)
  : Prop :=
  exists result,
    sched_wake_thread before thread_id = (after, result) /\
    rc = sr_rc result /\
    decision = sr_decision result /\
    sched_result_c_shape result.

Definition sched_block_thread_vst_post
    (before after : sched_state)
    (rc : sched_rc)
    (decision : sched_decision)
    (thread_id : Z)
  : Prop :=
  exists result,
    sched_block_thread before thread_id = (after, result) /\
    rc = sr_rc result /\
    decision = sr_decision result /\
    sched_result_c_shape result.

Definition sched_exit_thread_vst_post
    (before after : sched_state)
    (rc : sched_rc)
    (decision : sched_decision)
    (thread_id : Z)
  : Prop :=
  exists result,
    sched_exit_thread before thread_id = (after, result) /\
    rc = sr_rc result /\
    decision = sr_decision result /\
    sched_result_c_shape result.

Definition sched_on_timer_vst_post
    (before after : sched_state)
    (rc : sched_rc)
    (decision : sched_decision)
    (cpu_id : nat)
    (runtime_ns : Z)
  : Prop :=
  exists result,
    sched_on_timer before cpu_id runtime_ns = (after, result) /\
    rc = sr_rc result /\
    decision = sr_decision result /\
    sched_result_c_shape result.

Definition sched_pick_vst_post
    (before after : sched_state)
    (rc : sched_rc)
    (decision : sched_decision)
    (cpu_id : nat)
  : Prop :=
  exists result,
    sched_pick before cpu_id = (after, result) /\
    rc = sr_rc result /\
    decision = sr_decision result /\
    sched_result_c_shape result.

Definition sched_finish_current_vst_post
    (before after : sched_state)
    (rc : sched_rc)
    (decision : sched_decision)
    (cpu_id : nat)
  : Prop :=
  exists result,
    sched_finish_current before cpu_id = (after, result) /\
    rc = sr_rc result /\
    decision = sr_decision result /\
    sched_result_c_shape result.

Lemma sched_empty_cpu_c_shape :
  sched_cpu_c_shape sched_empty_cpu.
Proof.
  unfold sched_cpu_c_shape, sched_empty_cpu, c_bool_int_value,
    c_int64_value, c_int64_min, c_int64_max, no_thread_id.
  simpl.
  split; lia.
Qed.

Lemma sched_no_decision_c_shape :
  sched_decision_c_shape sched_no_decision.
Proof.
  unfold sched_decision_c_shape, sched_no_decision, c_int64_value,
    c_size_t_value, c_int64_min, c_int64_max, c_size_t_max,
    sched_no_cpu, sched_max_cpus, no_thread_id.
  simpl.
  repeat split; lia.
Qed.

Lemma sched_idle_decision_c_shape :
  forall cpu_id,
    c_size_t_value (Z.of_nat cpu_id) ->
    sched_decision_c_shape (sched_idle_decision cpu_id).
Proof.
  intros cpu_id Hcpu.
  destruct Hcpu as [Hcpu_min Hcpu_max].
  unfold sched_decision_c_shape, sched_idle_decision, c_int64_value,
    c_int64_min, c_int64_max, no_thread_id.
  simpl.
  repeat split; lia.
Qed.

Lemma sched_run_thread_decision_c_shape :
  forall cpu_id entity,
    c_size_t_value (Z.of_nat cpu_id) ->
    eevdf_entity_c_shape entity ->
    sched_decision_c_shape (sched_run_thread_decision cpu_id entity).
Proof.
  intros cpu_id entity Hcpu Hentity.
  destruct Hcpu as [Hcpu_min Hcpu_max].
  unfold sched_decision_c_shape, sched_run_thread_decision, c_int64_value,
    c_size_t_value, c_int64_min, c_int64_max, c_size_t_max in *.
  destruct Hentity as [Hthread [Hgen _]].
  destruct Hthread as [Hthread_min Hthread_max].
  destruct Hgen as [Hgen_min Hgen_max].
  simpl.
  repeat split.
  - lia.
  - lia.
  - exact Hcpu_min.
  - exact Hcpu_max.
  - exact Hthread_min.
  - exact Hthread_max.
  - exact Hgen_min.
  - exact Hgen_max.
Qed.
