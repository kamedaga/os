From Stdlib Require Import Bool.Bool Lists.List ZArith.ZArith.
From Pacha.Scheduling Require Import
  EevdfModel
  EevdfCharge
  EevdfPick
  EevdfTransitions
  KernelSchedModel.

Open Scope Z_scope.

Theorem kernel_empty_state_spec :
  forall cpu_count,
    ks_runqueues (kernel_sched_empty_state cpu_count) =
      repeat eevdf_empty_runqueue kernel_sched_max_cpus /\
    ks_cpus (kernel_sched_empty_state cpu_count) =
      repeat kernel_empty_cpu kernel_sched_max_cpus /\
    ks_cpu_count (kernel_sched_empty_state cpu_count) =
      (if (kernel_sched_max_cpus <? cpu_count)%nat
       then kernel_sched_max_cpus
       else cpu_count) /\
    ks_balance_cursor (kernel_sched_empty_state cpu_count) = 0%nat.
Proof.
  intros cpu_count.
  unfold kernel_sched_empty_state.
  repeat split.
Qed.

Theorem kernel_add_thread_invalid_cpu_spec :
  forall sched cpu_id thread_id generation weight slice_ns,
    kernel_valid_cpu sched cpu_id = false ->
    kernel_add_thread sched cpu_id thread_id generation weight slice_ns =
      (sched, kernel_sched_fail KernelSchedErrInvalid).
Proof.
  intros sched cpu_id thread_id generation weight slice_ns Hvalid.
  unfold kernel_add_thread.
  rewrite Hvalid.
  reflexivity.
Qed.

Theorem kernel_add_thread_success_spec :
  forall sched cpu_id rq thread_id generation weight slice_ns rq',
    kernel_valid_cpu sched cpu_id = true ->
    kernel_find_entity_cpu sched thread_id = None ->
    kernel_lookup_runqueue sched cpu_id = Some rq ->
    eevdf_add rq thread_id generation weight slice_ns = ok rq' ->
    kernel_add_thread sched cpu_id thread_id generation weight slice_ns =
      (kernel_replace_runqueue sched cpu_id rq',
       kernel_sched_ok kernel_no_decision).
Proof.
  intros sched cpu_id rq thread_id generation weight slice_ns rq'
    Hvalid Hfind Hlookup Hadd.
  unfold kernel_add_thread, kernel_apply_eevdf_result_to_cpu.
  rewrite Hvalid.
  rewrite Hfind.
  rewrite Hlookup.
  rewrite Hadd.
  reflexivity.
Qed.

Theorem kernel_add_thread_duplicate_spec :
  forall sched cpu_id owner_cpu thread_id generation weight slice_ns,
    kernel_valid_cpu sched cpu_id = true ->
    kernel_find_entity_cpu sched thread_id = Some owner_cpu ->
    kernel_add_thread sched cpu_id thread_id generation weight slice_ns =
      (sched, kernel_sched_fail KernelSchedErrInvalid).
Proof.
  intros sched cpu_id owner_cpu thread_id generation weight slice_ns
    Hvalid Hfind.
  unfold kernel_add_thread.
  rewrite Hvalid.
  rewrite Hfind.
  reflexivity.
Qed.

Theorem kernel_wake_thread_missing_spec :
  forall sched thread_id,
    kernel_find_entity_cpu sched thread_id = None ->
    kernel_wake_thread sched thread_id =
      (sched, kernel_sched_fail KernelSchedErrInvalid).
Proof.
  intros sched thread_id Hfind.
  unfold kernel_wake_thread.
  rewrite Hfind.
  reflexivity.
Qed.

Theorem kernel_wake_thread_success_spec :
  forall sched thread_id cpu_id rq rq',
    kernel_find_entity_cpu sched thread_id = Some cpu_id ->
    kernel_lookup_runqueue sched cpu_id = Some rq ->
    eevdf_wake rq thread_id = ok rq' ->
    kernel_wake_thread sched thread_id =
      (kernel_replace_runqueue sched cpu_id rq',
       kernel_sched_ok kernel_no_decision).
Proof.
  intros sched thread_id cpu_id rq rq' Hfind Hlookup Hwake.
  unfold kernel_wake_thread, kernel_apply_eevdf_result_to_cpu.
  rewrite Hfind.
  rewrite Hlookup.
  rewrite Hwake.
  reflexivity.
Qed.

Theorem kernel_block_thread_missing_spec :
  forall sched thread_id,
    kernel_find_entity_cpu sched thread_id = None ->
    kernel_block_thread sched thread_id =
      (sched, kernel_sched_fail KernelSchedErrInvalid).
Proof.
  intros sched thread_id Hfind.
  unfold kernel_block_thread.
  rewrite Hfind.
  reflexivity.
Qed.

Theorem kernel_block_thread_success_spec :
  forall sched thread_id cpu_id rq rq',
    kernel_find_entity_cpu sched thread_id = Some cpu_id ->
    kernel_lookup_runqueue sched cpu_id = Some rq ->
    eevdf_block rq thread_id = ok rq' ->
    kernel_block_thread sched thread_id =
      (kernel_clear_current_if_matches
         (kernel_replace_runqueue sched cpu_id rq')
         thread_id,
       kernel_sched_ok kernel_no_decision).
Proof.
  intros sched thread_id cpu_id rq rq' Hfind Hlookup Hblock.
  unfold kernel_block_thread.
  rewrite Hfind.
  rewrite Hlookup.
  rewrite Hblock.
  reflexivity.
Qed.

Theorem kernel_exit_thread_missing_spec :
  forall sched thread_id,
    kernel_find_entity_cpu sched thread_id = None ->
    kernel_exit_thread sched thread_id =
      (sched, kernel_sched_fail KernelSchedErrInvalid).
Proof.
  intros sched thread_id Hfind.
  unfold kernel_exit_thread.
  rewrite Hfind.
  reflexivity.
Qed.

Theorem kernel_exit_thread_success_spec :
  forall sched thread_id cpu_id rq rq',
    kernel_find_entity_cpu sched thread_id = Some cpu_id ->
    kernel_lookup_runqueue sched cpu_id = Some rq ->
    eevdf_exit rq thread_id = ok rq' ->
    kernel_exit_thread sched thread_id =
      (kernel_clear_current_if_matches
         (kernel_replace_runqueue sched cpu_id rq')
         thread_id,
       kernel_sched_ok kernel_no_decision).
Proof.
  intros sched thread_id cpu_id rq rq' Hfind Hlookup Hexit.
  unfold kernel_exit_thread.
  rewrite Hfind.
  rewrite Hlookup.
  rewrite Hexit.
  reflexivity.
Qed.

Theorem kernel_on_timer_invalid_cpu_spec :
  forall sched cpu_id runtime_ns,
    kernel_valid_cpu sched cpu_id = false ->
    kernel_on_timer sched cpu_id runtime_ns =
      (sched, kernel_sched_fail KernelSchedErrInvalid).
Proof.
  intros sched cpu_id runtime_ns Hvalid.
  unfold kernel_on_timer.
  rewrite Hvalid.
  reflexivity.
Qed.

Theorem kernel_on_timer_no_current_spec :
  forall sched cpu_id runtime_ns,
    kernel_valid_cpu sched cpu_id = true ->
    kernel_cpu_current sched cpu_id = None ->
    kernel_on_timer sched cpu_id runtime_ns =
      (sched, kernel_sched_ok kernel_no_decision).
Proof.
  intros sched cpu_id runtime_ns Hvalid Hcurrent.
  unfold kernel_on_timer.
  rewrite Hvalid.
  rewrite Hcurrent.
  reflexivity.
Qed.

Theorem kernel_on_timer_success_spec :
  forall sched cpu_id runtime_ns thread_id generation rq current_index rq',
    kernel_valid_cpu sched cpu_id = true ->
    kernel_cpu_current sched cpu_id = Some (thread_id, generation) ->
    kernel_lookup_runqueue sched cpu_id = Some rq ->
    kernel_current_entity_index rq thread_id generation = Some current_index ->
    eevdf_charge rq thread_id runtime_ns = ok rq' ->
    kernel_on_timer sched cpu_id runtime_ns =
      (kernel_replace_runqueue sched cpu_id rq',
       kernel_sched_ok kernel_no_decision).
Proof.
  intros sched cpu_id runtime_ns thread_id generation rq current_index rq'
    Hvalid Hcurrent Hlookup Hcurrent_entity Hcharge.
  unfold kernel_on_timer, kernel_apply_eevdf_result_to_cpu.
  rewrite Hvalid.
  rewrite Hcurrent.
  rewrite Hlookup.
  rewrite Hcurrent_entity.
  rewrite Hcharge.
  reflexivity.
Qed.

Theorem kernel_on_timer_stale_current_spec :
  forall sched cpu_id runtime_ns thread_id generation rq,
    kernel_valid_cpu sched cpu_id = true ->
    kernel_cpu_current sched cpu_id = Some (thread_id, generation) ->
    kernel_lookup_runqueue sched cpu_id = Some rq ->
    kernel_current_entity_index rq thread_id generation = None ->
    kernel_on_timer sched cpu_id runtime_ns =
      (kernel_clear_cpu_current sched cpu_id,
       kernel_sched_ok kernel_no_decision).
Proof.
  intros sched cpu_id runtime_ns thread_id generation rq
    Hvalid Hcurrent Hlookup Hcurrent_entity.
  unfold kernel_on_timer.
  rewrite Hvalid.
  rewrite Hcurrent.
  rewrite Hlookup.
  rewrite Hcurrent_entity.
  reflexivity.
Qed.

Theorem kernel_pick_invalid_cpu_spec :
  forall sched cpu_id,
    kernel_valid_cpu sched cpu_id = false ->
    kernel_pick_cpu sched cpu_id =
      (sched, kernel_sched_fail KernelSchedErrInvalid).
Proof.
  intros sched cpu_id Hvalid.
  unfold kernel_pick_cpu.
  rewrite Hvalid.
  reflexivity.
Qed.

Theorem kernel_pick_busy_cpu_spec :
  forall sched cpu_id,
    kernel_valid_cpu sched cpu_id = true ->
    kernel_cpu_has_current sched cpu_id = true ->
    kernel_pick_cpu sched cpu_id =
      (sched, kernel_sched_fail KernelSchedErrState).
Proof.
  intros sched cpu_id Hvalid Hcurrent.
  unfold kernel_pick_cpu.
  rewrite Hvalid.
  rewrite Hcurrent.
  reflexivity.
Qed.

Theorem kernel_pick_idle_spec :
  forall sched cpu_id rq rq_after_pick,
    kernel_valid_cpu sched cpu_id = true ->
    kernel_cpu_has_current sched cpu_id = false ->
    kernel_lookup_runqueue sched cpu_id = Some rq ->
    eevdf_pick rq = (rq_after_pick, None) ->
    kernel_pick_cpu sched cpu_id =
      (kernel_set_activation_pending
         (kernel_replace_runqueue sched cpu_id rq_after_pick)
         cpu_id
         false,
       kernel_sched_ok (kernel_idle_decision cpu_id)).
Proof.
  intros sched cpu_id rq rq_after_pick Hvalid Hcurrent Hlookup Hpick.
  unfold kernel_pick_cpu.
  rewrite Hvalid.
  rewrite Hcurrent.
  rewrite Hlookup.
  rewrite Hpick.
  reflexivity.
Qed.

Theorem kernel_pick_run_thread_spec :
  forall sched cpu_id rq rq_after_pick index entity rq',
    kernel_valid_cpu sched cpu_id = true ->
    kernel_cpu_has_current sched cpu_id = false ->
    kernel_lookup_runqueue sched cpu_id = Some rq ->
    eevdf_pick rq = (rq_after_pick, Some (index, entity)) ->
    eevdf_mark_running rq_after_pick (ee_thread_id entity) = ok rq' ->
    kernel_pick_cpu sched cpu_id =
      (kernel_set_cpu_current
         (kernel_set_activation_pending
           (kernel_replace_runqueue
             (kernel_replace_runqueue sched cpu_id rq_after_pick)
             cpu_id
             rq')
           cpu_id
           false)
         cpu_id
         (ee_thread_id entity)
         (ee_generation entity),
       kernel_sched_ok (kernel_run_thread_decision cpu_id entity)).
Proof.
  intros sched cpu_id rq rq_after_pick index entity rq'
    Hvalid Hcurrent Hlookup Hpick Hmark.
  unfold kernel_pick_cpu.
  rewrite Hvalid.
  rewrite Hcurrent.
  rewrite Hlookup.
  rewrite Hpick.
  rewrite Hmark.
  reflexivity.
Qed.

Theorem kernel_finish_current_invalid_cpu_spec :
  forall sched cpu_id,
    kernel_valid_cpu sched cpu_id = false ->
    kernel_finish_current sched cpu_id =
      (sched, kernel_sched_fail KernelSchedErrInvalid).
Proof.
  intros sched cpu_id Hvalid.
  unfold kernel_finish_current.
  rewrite Hvalid.
  reflexivity.
Qed.

Theorem kernel_finish_current_no_current_spec :
  forall sched cpu_id,
    kernel_valid_cpu sched cpu_id = true ->
    kernel_cpu_current sched cpu_id = None ->
    kernel_finish_current sched cpu_id =
      (sched, kernel_sched_ok kernel_no_decision).
Proof.
  intros sched cpu_id Hvalid Hcurrent.
  unfold kernel_finish_current.
  rewrite Hvalid.
  rewrite Hcurrent.
  reflexivity.
Qed.

Theorem kernel_finish_current_success_spec :
  forall sched cpu_id thread_id generation rq current_index rq',
    kernel_valid_cpu sched cpu_id = true ->
    kernel_cpu_current sched cpu_id = Some (thread_id, generation) ->
    kernel_lookup_runqueue sched cpu_id = Some rq ->
    kernel_current_entity_index rq thread_id generation = Some current_index ->
    eevdf_requeue_running rq thread_id = ok rq' ->
    kernel_finish_current sched cpu_id =
      (kernel_clear_cpu_current
         (kernel_replace_runqueue sched cpu_id rq')
         cpu_id,
       kernel_sched_ok kernel_no_decision).
Proof.
  intros sched cpu_id thread_id generation rq current_index rq'
    Hvalid Hcurrent Hlookup Hcurrent_entity Hrequeue.
  unfold kernel_finish_current.
  rewrite Hvalid.
  rewrite Hcurrent.
  rewrite Hlookup.
  rewrite Hcurrent_entity.
  rewrite Hrequeue.
  reflexivity.
Qed.

Theorem kernel_finish_current_stale_current_spec :
  forall sched cpu_id thread_id generation rq,
    kernel_valid_cpu sched cpu_id = true ->
    kernel_cpu_current sched cpu_id = Some (thread_id, generation) ->
    kernel_lookup_runqueue sched cpu_id = Some rq ->
    kernel_current_entity_index rq thread_id generation = None ->
    kernel_finish_current sched cpu_id =
      (kernel_clear_cpu_current sched cpu_id,
       kernel_sched_ok kernel_no_decision).
Proof.
  intros sched cpu_id thread_id generation rq
    Hvalid Hcurrent Hlookup Hcurrent_entity.
  unfold kernel_finish_current.
  rewrite Hvalid.
  rewrite Hcurrent.
  rewrite Hlookup.
  rewrite Hcurrent_entity.
  reflexivity.
Qed.

Theorem kernel_handoff_invalid_cpu_spec :
  forall sched cpu_id target_thread_id,
    kernel_valid_cpu sched cpu_id = false ->
    kernel_handoff_to_thread_on_cpu sched cpu_id target_thread_id =
      (sched, kernel_sched_fail KernelSchedErrInvalid).
Proof.
  intros sched cpu_id target_thread_id Hvalid.
  unfold kernel_handoff_to_thread_on_cpu.
  rewrite Hvalid.
  reflexivity.
Qed.

Theorem kernel_handoff_no_current_spec :
  forall sched cpu_id target_thread_id,
    kernel_valid_cpu sched cpu_id = true ->
    kernel_cpu_current sched cpu_id = None ->
    kernel_handoff_to_thread_on_cpu sched cpu_id target_thread_id =
      (sched, kernel_sched_fail KernelSchedErrState).
Proof.
  intros sched cpu_id target_thread_id Hvalid Hcurrent.
  unfold kernel_handoff_to_thread_on_cpu.
  rewrite Hvalid.
  rewrite Hcurrent.
  reflexivity.
Qed.

Theorem kernel_handoff_missing_target_spec :
  forall sched cpu_id target_thread_id current_thread_id current_generation rq current_index,
    kernel_valid_cpu sched cpu_id = true ->
    kernel_cpu_current sched cpu_id = Some (current_thread_id, current_generation) ->
    kernel_lookup_runqueue sched cpu_id = Some rq ->
    kernel_current_entity_index rq current_thread_id current_generation = Some current_index ->
    find_entity_index rq target_thread_id = None ->
    kernel_handoff_to_thread_on_cpu sched cpu_id target_thread_id =
      (sched, kernel_sched_fail KernelSchedErrInvalid).
Proof.
  intros sched cpu_id target_thread_id current_thread_id current_generation rq current_index
    Hvalid Hcurrent Hlookup Hcurrent_entity Htarget.
  unfold kernel_handoff_to_thread_on_cpu.
  rewrite Hvalid.
  rewrite Hcurrent.
  rewrite Hlookup.
  rewrite Hcurrent_entity.
  rewrite Htarget.
  reflexivity.
Qed.

Theorem kernel_handoff_success_spec :
  forall sched cpu_id target_thread_id current_thread_id current_generation
    rq current_index target_index target rq_after_finish rq_after_target,
    kernel_valid_cpu sched cpu_id = true ->
    kernel_cpu_current sched cpu_id = Some (current_thread_id, current_generation) ->
    kernel_lookup_runqueue sched cpu_id = Some rq ->
    kernel_current_entity_index rq current_thread_id current_generation = Some current_index ->
    find_entity_index rq target_thread_id = Some target_index ->
    lookup_entity rq target_index = Some target ->
    ee_state target = ERunnable ->
    eevdf_requeue_running rq current_thread_id = ok rq_after_finish ->
    eevdf_mark_running rq_after_finish target_thread_id = ok rq_after_target ->
    kernel_handoff_to_thread_on_cpu sched cpu_id target_thread_id =
      (kernel_set_cpu_current
        (kernel_replace_runqueue sched cpu_id rq_after_target)
        cpu_id
        (ee_thread_id target)
        (ee_generation target),
       kernel_sched_ok (kernel_run_thread_decision cpu_id target)).
Proof.
  intros sched cpu_id target_thread_id current_thread_id current_generation
    rq current_index target_index target rq_after_finish rq_after_target
    Hvalid Hcurrent Hlookup Hcurrent_entity Hfind Htarget Hrunnable Hfinish Hmark.
  unfold kernel_handoff_to_thread_on_cpu.
  rewrite Hvalid.
  rewrite Hcurrent.
  rewrite Hlookup.
  rewrite Hcurrent_entity.
  rewrite Hfind.
  rewrite Htarget.
  rewrite Hrunnable.
  rewrite Hfinish.
  rewrite Hmark.
  reflexivity.
Qed.

Theorem kernel_request_activation_invalid_cpu_spec :
  forall sched cpu_id,
    kernel_valid_cpu sched cpu_id = false ->
    kernel_request_activation sched cpu_id =
      (sched, kernel_sched_fail KernelSchedErrInvalid).
Proof.
  intros sched cpu_id Hvalid.
  unfold kernel_request_activation.
  rewrite Hvalid.
  reflexivity.
Qed.

Theorem kernel_request_activation_success_spec :
  forall sched cpu_id,
    kernel_valid_cpu sched cpu_id = true ->
    kernel_request_activation sched cpu_id =
      (kernel_set_activation_pending sched cpu_id true,
       kernel_sched_ok kernel_no_decision).
Proof.
  intros sched cpu_id Hvalid.
  unfold kernel_request_activation.
  rewrite Hvalid.
  reflexivity.
Qed.

Theorem kernel_claim_activation_spec :
  forall sched cpu_id,
    kernel_claim_activation sched cpu_id =
      if negb (kernel_valid_cpu sched cpu_id) then
        (sched, kernel_sched_fail KernelSchedErrInvalid)
      else kernel_pick_cpu sched cpu_id.
Proof.
  intros sched cpu_id.
  unfold kernel_claim_activation.
  reflexivity.
Qed.

Theorem kernel_migrate_invalid_cpu_spec :
  forall sched src_cpu dst_cpu thread_id,
    orb
      (negb (kernel_valid_cpu sched src_cpu))
      (negb (kernel_valid_cpu sched dst_cpu)) = true ->
    kernel_migrate_runnable sched src_cpu dst_cpu thread_id =
      (sched, kernel_sched_fail KernelSchedErrInvalid).
Proof.
  intros sched src_cpu dst_cpu thread_id Hvalid.
  unfold kernel_migrate_runnable.
  rewrite Hvalid.
  reflexivity.
Qed.

Theorem kernel_migrate_same_cpu_spec :
  forall sched src_cpu thread_id,
    orb
      (negb (kernel_valid_cpu sched src_cpu))
      (negb (kernel_valid_cpu sched src_cpu)) = false ->
    kernel_migrate_runnable sched src_cpu src_cpu thread_id =
      (sched, kernel_sched_fail KernelSchedErrInvalid).
Proof.
  intros sched src_cpu thread_id Hvalid.
  unfold kernel_migrate_runnable.
  rewrite Hvalid.
  rewrite Nat.eqb_refl.
  reflexivity.
Qed.

Theorem kernel_migrate_missing_thread_spec :
  forall sched src_cpu dst_cpu thread_id src dst,
    orb
      (negb (kernel_valid_cpu sched src_cpu))
      (negb (kernel_valid_cpu sched dst_cpu)) = false ->
    Nat.eqb src_cpu dst_cpu = false ->
    kernel_lookup_runqueue sched src_cpu = Some src ->
    kernel_lookup_runqueue sched dst_cpu = Some dst ->
    find_entity_index src thread_id = None ->
    kernel_migrate_runnable sched src_cpu dst_cpu thread_id =
      (sched, kernel_sched_fail KernelSchedErrInvalid).
Proof.
  intros sched src_cpu dst_cpu thread_id src dst
    Hvalid Hneq Hsrc Hdst Hfind.
  unfold kernel_migrate_runnable.
  rewrite Hvalid.
  rewrite Hneq.
  rewrite Hsrc.
  rewrite Hdst.
  rewrite Hfind.
  reflexivity.
Qed.

Theorem kernel_migrate_non_runnable_spec :
  forall sched src_cpu dst_cpu thread_id src dst index entity,
    orb
      (negb (kernel_valid_cpu sched src_cpu))
      (negb (kernel_valid_cpu sched dst_cpu)) = false ->
    Nat.eqb src_cpu dst_cpu = false ->
    kernel_lookup_runqueue sched src_cpu = Some src ->
    kernel_lookup_runqueue sched dst_cpu = Some dst ->
    find_entity_index src thread_id = Some index ->
    lookup_entity src index = Some entity ->
    ee_state entity <> ERunnable ->
    kernel_migrate_runnable sched src_cpu dst_cpu thread_id =
      (sched, kernel_sched_fail KernelSchedErrState).
Proof.
  intros sched src_cpu dst_cpu thread_id src dst index entity
    Hvalid Hneq Hsrc Hdst Hfind Hlookup Hstate.
  unfold kernel_migrate_runnable.
  rewrite Hvalid.
  rewrite Hneq.
  rewrite Hsrc.
  rewrite Hdst.
  rewrite Hfind.
  rewrite Hlookup.
  destruct (ee_state entity); try reflexivity; contradiction.
Qed.

Theorem kernel_migrate_destination_full_spec :
  forall sched src_cpu dst_cpu thread_id src dst index entity,
    orb
      (negb (kernel_valid_cpu sched src_cpu))
      (negb (kernel_valid_cpu sched dst_cpu)) = false ->
    Nat.eqb src_cpu dst_cpu = false ->
    kernel_lookup_runqueue sched src_cpu = Some src ->
    kernel_lookup_runqueue sched dst_cpu = Some dst ->
    find_entity_index src thread_id = Some index ->
    lookup_entity src index = Some entity ->
    ee_state entity = ERunnable ->
    (er_entity_count dst <? eevdf_max_entities)%nat = false ->
    kernel_migrate_runnable sched src_cpu dst_cpu thread_id =
      (sched, kernel_sched_fail KernelSchedErrFull).
Proof.
  intros sched src_cpu dst_cpu thread_id src dst index entity
    Hvalid Hneq Hsrc Hdst Hfind Hlookup Hstate Hspace.
  unfold kernel_migrate_runnable.
  rewrite Hvalid.
  rewrite Hneq.
  rewrite Hsrc.
  rewrite Hdst.
  rewrite Hfind.
  rewrite Hlookup.
  rewrite Hstate.
  rewrite Hspace.
  reflexivity.
Qed.

Theorem kernel_migrate_duplicate_destination_spec :
  forall sched src_cpu dst_cpu thread_id src dst index entity duplicate_index,
    orb
      (negb (kernel_valid_cpu sched src_cpu))
      (negb (kernel_valid_cpu sched dst_cpu)) = false ->
    Nat.eqb src_cpu dst_cpu = false ->
    kernel_lookup_runqueue sched src_cpu = Some src ->
    kernel_lookup_runqueue sched dst_cpu = Some dst ->
    find_entity_index src thread_id = Some index ->
    lookup_entity src index = Some entity ->
    ee_state entity = ERunnable ->
    (er_entity_count dst <? eevdf_max_entities)%nat = true ->
    find_entity_index dst thread_id = Some duplicate_index ->
    kernel_migrate_runnable sched src_cpu dst_cpu thread_id =
      (sched, kernel_sched_fail KernelSchedErrInvalid).
Proof.
  intros sched src_cpu dst_cpu thread_id src dst index entity duplicate_index
    Hvalid Hneq Hsrc Hdst Hfind Hlookup Hstate Hspace Hdup.
  unfold kernel_migrate_runnable.
  rewrite Hvalid.
  rewrite Hneq.
  rewrite Hsrc.
  rewrite Hdst.
  rewrite Hfind.
  rewrite Hlookup.
  rewrite Hstate.
  rewrite Hspace.
  rewrite Hdup.
  reflexivity.
Qed.

Theorem kernel_migrate_refresh_overflow_spec :
  forall sched src_cpu dst_cpu thread_id src dst index entity,
    orb
      (negb (kernel_valid_cpu sched src_cpu))
      (negb (kernel_valid_cpu sched dst_cpu)) = false ->
    Nat.eqb src_cpu dst_cpu = false ->
    kernel_lookup_runqueue sched src_cpu = Some src ->
    kernel_lookup_runqueue sched dst_cpu = Some dst ->
    find_entity_index src thread_id = Some index ->
    lookup_entity src index = Some entity ->
    ee_state entity = ERunnable ->
    (er_entity_count dst <? eevdf_max_entities)%nat = true ->
    find_entity_index dst thread_id = None ->
    entity_as_migrated_runnable dst entity = None ->
    kernel_migrate_runnable sched src_cpu dst_cpu thread_id =
      (sched, kernel_sched_fail KernelSchedErrOverflow).
Proof.
  intros sched src_cpu dst_cpu thread_id src dst index entity
    Hvalid Hneq Hsrc Hdst Hfind Hlookup Hstate Hspace Hdup Hmoved.
  unfold kernel_migrate_runnable.
  rewrite Hvalid.
  rewrite Hneq.
  rewrite Hsrc.
  rewrite Hdst.
  rewrite Hfind.
  rewrite Hlookup.
  rewrite Hstate.
  rewrite Hspace.
  rewrite Hdup.
  rewrite Hmoved.
  reflexivity.
Qed.

Theorem kernel_migrate_success_spec :
  forall sched src_cpu dst_cpu thread_id src dst index entity moved,
    orb
      (negb (kernel_valid_cpu sched src_cpu))
      (negb (kernel_valid_cpu sched dst_cpu)) = false ->
    Nat.eqb src_cpu dst_cpu = false ->
    kernel_lookup_runqueue sched src_cpu = Some src ->
    kernel_lookup_runqueue sched dst_cpu = Some dst ->
    find_entity_index src thread_id = Some index ->
    lookup_entity src index = Some entity ->
    ee_state entity = ERunnable ->
    (er_entity_count dst <? eevdf_max_entities)%nat = true ->
    find_entity_index dst thread_id = None ->
    entity_as_migrated_runnable dst entity = Some moved ->
    let src' := remove_entity_from_runqueue src thread_id in
    let dst' := refresh_runqueue (append_entity dst moved) in
    kernel_migrate_runnable sched src_cpu dst_cpu thread_id =
      (kernel_replace_runqueue
        (kernel_replace_runqueue sched src_cpu src')
        dst_cpu
        dst',
       kernel_sched_ok kernel_no_decision).
Proof.
  intros sched src_cpu dst_cpu thread_id src dst index entity moved
    Hvalid Hneq Hsrc Hdst Hfind Hlookup Hstate Hspace Hdup Hmoved.
  unfold kernel_migrate_runnable.
  rewrite Hvalid.
  rewrite Hneq.
  rewrite Hsrc.
  rewrite Hdst.
  rewrite Hfind.
  rewrite Hlookup.
  rewrite Hstate.
  rewrite Hspace.
  rewrite Hdup.
  rewrite Hmoved.
  reflexivity.
Qed.
