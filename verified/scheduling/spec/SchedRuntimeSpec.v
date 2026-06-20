From Stdlib Require Import Bool.Bool Lists.List ZArith.ZArith.
From Pacha.Scheduling Require Import
  EevdfModel
  EevdfPick
  EevdfTransitions
  SchedRuntimeModel.

Open Scope Z_scope.

Theorem sched_empty_state_spec :
  forall cpu_count,
    ss_runqueue (sched_empty_state cpu_count) = eevdf_empty_runqueue /\
    ss_cpus (sched_empty_state cpu_count) =
      repeat sched_empty_cpu sched_max_cpus /\
    ss_cpu_count (sched_empty_state cpu_count) =
      if (sched_max_cpus <? cpu_count)%nat then sched_max_cpus else cpu_count.
Proof.
  intros cpu_count.
  unfold sched_empty_state.
  repeat split.
Qed.

Theorem sched_add_thread_success_spec :
  forall sched thread_id generation weight slice_ns rq',
    eevdf_add
      (ss_runqueue sched)
      thread_id
      generation
      weight
      slice_ns =
      ok rq' ->
    sched_add_thread sched thread_id generation weight slice_ns =
      (with_runqueue sched rq', sched_ok sched_no_decision).
Proof.
  intros sched thread_id generation weight slice_ns rq' Hadd.
  unfold sched_add_thread, sched_apply_eevdf_result.
  rewrite Hadd.
  reflexivity.
Qed.

Theorem sched_add_thread_failure_spec :
  forall sched thread_id generation weight slice_ns rc,
    rc <> EevdfOk ->
    eevdf_add
      (ss_runqueue sched)
      thread_id
      generation
      weight
      slice_ns =
      fail rc (ss_runqueue sched) ->
    sched_add_thread sched thread_id generation weight slice_ns =
      (sched, sched_fail (map_eevdf_rc rc)).
Proof.
  intros sched thread_id generation weight slice_ns rc Hnot_ok Hadd.
  unfold sched_add_thread, sched_apply_eevdf_result.
  rewrite Hadd.
  destruct rc; try contradiction; reflexivity.
Qed.

Theorem sched_wake_thread_success_spec :
  forall sched thread_id rq',
    eevdf_wake (ss_runqueue sched) thread_id = ok rq' ->
    sched_wake_thread sched thread_id =
      (with_runqueue sched rq', sched_ok sched_no_decision).
Proof.
  intros sched thread_id rq' Hwake.
  unfold sched_wake_thread, sched_apply_eevdf_result.
  rewrite Hwake.
  reflexivity.
Qed.

Theorem sched_wake_thread_failure_spec :
  forall sched thread_id rc,
    rc <> EevdfOk ->
    eevdf_wake (ss_runqueue sched) thread_id =
      fail rc (ss_runqueue sched) ->
    sched_wake_thread sched thread_id =
      (sched, sched_fail (map_eevdf_rc rc)).
Proof.
  intros sched thread_id rc Hnot_ok Hwake.
  unfold sched_wake_thread, sched_apply_eevdf_result.
  rewrite Hwake.
  destruct rc; try contradiction; reflexivity.
Qed.

Theorem sched_block_thread_success_spec :
  forall sched thread_id rq',
    eevdf_block (ss_runqueue sched) thread_id = ok rq' ->
    sched_block_thread sched thread_id =
      (clear_current_if_matches (with_runqueue sched rq') thread_id,
       sched_ok sched_no_decision).
Proof.
  intros sched thread_id rq' Hblock.
  unfold sched_block_thread.
  rewrite Hblock.
  reflexivity.
Qed.

Theorem sched_block_thread_failure_spec :
  forall sched thread_id rc,
    rc <> EevdfOk ->
    eevdf_block (ss_runqueue sched) thread_id =
      fail rc (ss_runqueue sched) ->
    sched_block_thread sched thread_id =
      (sched, sched_fail (map_eevdf_rc rc)).
Proof.
  intros sched thread_id rc Hnot_ok Hblock.
  unfold sched_block_thread.
  rewrite Hblock.
  destruct rc; try contradiction; reflexivity.
Qed.

Theorem sched_exit_thread_success_spec :
  forall sched thread_id rq',
    eevdf_exit (ss_runqueue sched) thread_id = ok rq' ->
    sched_exit_thread sched thread_id =
      (clear_current_if_matches (with_runqueue sched rq') thread_id,
       sched_ok sched_no_decision).
Proof.
  intros sched thread_id rq' Hexit.
  unfold sched_exit_thread.
  rewrite Hexit.
  reflexivity.
Qed.

Theorem sched_exit_thread_failure_spec :
  forall sched thread_id rc,
    rc <> EevdfOk ->
    eevdf_exit (ss_runqueue sched) thread_id =
      fail rc (ss_runqueue sched) ->
    sched_exit_thread sched thread_id =
      (sched, sched_fail (map_eevdf_rc rc)).
Proof.
  intros sched thread_id rc Hnot_ok Hexit.
  unfold sched_exit_thread.
  rewrite Hexit.
  destruct rc; try contradiction; reflexivity.
Qed.

Theorem sched_on_timer_invalid_cpu_spec :
  forall sched cpu_id runtime_ns,
    valid_cpu sched cpu_id = false ->
    sched_on_timer sched cpu_id runtime_ns =
      (sched, sched_fail SchedErrInvalid).
Proof.
  intros sched cpu_id runtime_ns Hvalid.
  unfold sched_on_timer.
  rewrite Hvalid.
  reflexivity.
Qed.

Theorem sched_on_timer_no_current_spec :
  forall sched cpu_id runtime_ns,
    valid_cpu sched cpu_id = true ->
    cpu_current_thread_id sched cpu_id = None ->
    sched_on_timer sched cpu_id runtime_ns =
      (sched, sched_ok sched_no_decision).
Proof.
  intros sched cpu_id runtime_ns Hvalid Hcurrent.
  unfold sched_on_timer.
  rewrite Hvalid.
  rewrite Hcurrent.
  reflexivity.
Qed.

Theorem sched_on_timer_success_spec :
  forall sched cpu_id runtime_ns thread_id rq',
    valid_cpu sched cpu_id = true ->
    cpu_current_thread_id sched cpu_id = Some thread_id ->
    eevdf_charge (ss_runqueue sched) thread_id runtime_ns = ok rq' ->
    sched_on_timer sched cpu_id runtime_ns =
      (with_runqueue sched rq', sched_ok sched_no_decision).
Proof.
  intros sched cpu_id runtime_ns thread_id rq' Hvalid Hcurrent Hcharge.
  unfold sched_on_timer, sched_apply_eevdf_result.
  rewrite Hvalid.
  rewrite Hcurrent.
  rewrite Hcharge.
  reflexivity.
Qed.

Theorem sched_on_timer_failure_spec :
  forall sched cpu_id runtime_ns thread_id rc,
    valid_cpu sched cpu_id = true ->
    cpu_current_thread_id sched cpu_id = Some thread_id ->
    rc <> EevdfOk ->
    eevdf_charge (ss_runqueue sched) thread_id runtime_ns =
      fail rc (ss_runqueue sched) ->
    sched_on_timer sched cpu_id runtime_ns =
      (sched, sched_fail (map_eevdf_rc rc)).
Proof.
  intros sched cpu_id runtime_ns thread_id rc Hvalid Hcurrent Hnot_ok Hcharge.
  unfold sched_on_timer, sched_apply_eevdf_result.
  rewrite Hvalid.
  rewrite Hcurrent.
  rewrite Hcharge.
  destruct rc; try contradiction; reflexivity.
Qed.

Theorem sched_pick_invalid_cpu_spec :
  forall sched cpu_id,
    valid_cpu sched cpu_id = false ->
    sched_pick sched cpu_id =
      (sched, sched_fail SchedErrInvalid).
Proof.
  intros sched cpu_id Hvalid.
  unfold sched_pick.
  rewrite Hvalid.
  reflexivity.
Qed.

Theorem sched_pick_cpu_busy_spec :
  forall sched cpu_id,
    valid_cpu sched cpu_id = true ->
    cpu_has_current sched cpu_id = true ->
    sched_pick sched cpu_id =
      (sched, sched_fail SchedErrState).
Proof.
  intros sched cpu_id Hvalid Hcurrent.
  unfold sched_pick.
  rewrite Hvalid.
  rewrite Hcurrent.
  reflexivity.
Qed.

Theorem sched_pick_idle_spec :
  forall sched cpu_id rq_after_pick,
    valid_cpu sched cpu_id = true ->
    cpu_has_current sched cpu_id = false ->
    eevdf_pick (ss_runqueue sched) = (rq_after_pick, None) ->
    sched_pick sched cpu_id =
      (with_runqueue sched rq_after_pick,
       sched_ok (sched_idle_decision cpu_id)).
Proof.
  intros sched cpu_id rq_after_pick Hvalid Hcurrent Hpick.
  unfold sched_pick.
  rewrite Hvalid.
  rewrite Hcurrent.
  rewrite Hpick.
  reflexivity.
Qed.

Theorem sched_pick_run_thread_spec :
  forall sched cpu_id rq_after_pick index entity rq_after_mark,
    valid_cpu sched cpu_id = true ->
    cpu_has_current sched cpu_id = false ->
    eevdf_pick (ss_runqueue sched) =
      (rq_after_pick, Some (index, entity)) ->
    eevdf_mark_running rq_after_pick (ee_thread_id entity) =
      ok rq_after_mark ->
    sched_pick sched cpu_id =
      (set_cpu_current (with_runqueue (with_runqueue sched rq_after_pick)
        rq_after_mark) cpu_id (ee_thread_id entity),
       sched_ok (sched_run_thread_decision cpu_id entity)).
Proof.
  intros sched cpu_id rq_after_pick index entity rq_after_mark
    Hvalid Hcurrent Hpick Hmark.
  unfold sched_pick.
  rewrite Hvalid.
  rewrite Hcurrent.
  rewrite Hpick.
  simpl.
  rewrite Hmark.
  reflexivity.
Qed.

Theorem sched_pick_mark_failure_spec :
  forall sched cpu_id rq_after_pick index entity rc,
    valid_cpu sched cpu_id = true ->
    cpu_has_current sched cpu_id = false ->
    eevdf_pick (ss_runqueue sched) =
      (rq_after_pick, Some (index, entity)) ->
    rc <> EevdfOk ->
    eevdf_mark_running rq_after_pick (ee_thread_id entity) =
      fail rc rq_after_pick ->
    sched_pick sched cpu_id =
      (with_runqueue sched rq_after_pick, sched_fail (map_eevdf_rc rc)).
Proof.
  intros sched cpu_id rq_after_pick index entity rc
    Hvalid Hcurrent Hpick Hnot_ok Hmark.
  unfold sched_pick.
  rewrite Hvalid.
  rewrite Hcurrent.
  rewrite Hpick.
  simpl.
  rewrite Hmark.
  destruct rc; try contradiction; reflexivity.
Qed.

Theorem sched_finish_current_invalid_cpu_spec :
  forall sched cpu_id,
    valid_cpu sched cpu_id = false ->
    sched_finish_current sched cpu_id =
      (sched, sched_fail SchedErrInvalid).
Proof.
  intros sched cpu_id Hvalid.
  unfold sched_finish_current.
  rewrite Hvalid.
  reflexivity.
Qed.

Theorem sched_finish_current_no_current_spec :
  forall sched cpu_id,
    valid_cpu sched cpu_id = true ->
    cpu_current_thread_id sched cpu_id = None ->
    sched_finish_current sched cpu_id =
      (sched, sched_ok sched_no_decision).
Proof.
  intros sched cpu_id Hvalid Hcurrent.
  unfold sched_finish_current.
  rewrite Hvalid.
  rewrite Hcurrent.
  reflexivity.
Qed.

Theorem sched_finish_current_success_spec :
  forall sched cpu_id thread_id rq',
    valid_cpu sched cpu_id = true ->
    cpu_current_thread_id sched cpu_id = Some thread_id ->
    eevdf_requeue_running (ss_runqueue sched) thread_id = ok rq' ->
    sched_finish_current sched cpu_id =
      (clear_cpu_current (with_runqueue sched rq') cpu_id,
       sched_ok sched_no_decision).
Proof.
  intros sched cpu_id thread_id rq' Hvalid Hcurrent Hrequeue.
  unfold sched_finish_current.
  rewrite Hvalid.
  rewrite Hcurrent.
  rewrite Hrequeue.
  reflexivity.
Qed.

Theorem sched_finish_current_failure_spec :
  forall sched cpu_id thread_id rc,
    valid_cpu sched cpu_id = true ->
    cpu_current_thread_id sched cpu_id = Some thread_id ->
    rc <> EevdfOk ->
    eevdf_requeue_running (ss_runqueue sched) thread_id =
      fail rc (ss_runqueue sched) ->
    sched_finish_current sched cpu_id =
      (sched, sched_fail (map_eevdf_rc rc)).
Proof.
  intros sched cpu_id thread_id rc Hvalid Hcurrent Hnot_ok Hrequeue.
  unfold sched_finish_current.
  rewrite Hvalid.
  rewrite Hcurrent.
  rewrite Hrequeue.
  destruct rc; try contradiction; reflexivity.
Qed.
