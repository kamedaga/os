From Stdlib Require Import Bool.Bool Lia Lists.List ZArith.ZArith.
From Pacha.Scheduling Require Import ProtocolModel EevdfModel EevdfInvariants.

Import ListNotations.
Open Scope Z_scope.

Definition eevdf_empty_entity : eevdf_entity :=
  {|
    ee_thread_id := no_thread_id;
    ee_generation := 0;
    ee_weight := 0;
    ee_slice_ns := 0;
    ee_service_ns := 0;
    ee_vruntime := 0;
    ee_eligible_time := 0;
    ee_deadline := 0;
    ee_state := EEmpty;
  |}.

Definition eevdf_empty_runqueue : eevdf_runqueue :=
  {|
    er_entities := repeat eevdf_empty_entity eevdf_max_entities;
    er_entity_count := 0%nat;
    er_runnable_count := 0%nat;
    er_virtual_time := 0;
    er_min_vruntime := 0;
  |}.

Definition eevdf_valid_entity_params
    (weight slice_ns : Z)
  : bool :=
  andb (valid_positive weight) (valid_positive slice_ns).

Definition eevdf_make_entity
    (rq : eevdf_runqueue)
    (thread_id generation weight slice_ns : Z)
  : option eevdf_entity :=
  let entity :=
    {|
      ee_thread_id := thread_id;
      ee_generation := generation;
      ee_weight := weight;
      ee_slice_ns := slice_ns;
      ee_service_ns := 0;
      ee_vruntime := er_min_vruntime rq;
      ee_eligible_time := er_min_vruntime rq;
      ee_deadline := er_min_vruntime rq;
      ee_state := ERunnable;
    |}
  in
  refresh_deadline entity (er_min_vruntime rq).

Definition append_entity
    (rq : eevdf_runqueue)
    (entity : eevdf_entity)
  : eevdf_runqueue :=
  {|
    er_entities := replace_nth (er_entities rq) (er_entity_count rq) entity;
    er_entity_count := S (er_entity_count rq);
    er_runnable_count := er_runnable_count rq;
    er_virtual_time := er_virtual_time rq;
    er_min_vruntime := er_min_vruntime rq;
  |}.

Definition eevdf_reset
    (_rq : eevdf_runqueue)
  : eevdf_result :=
  ok eevdf_empty_runqueue.

Definition eevdf_add
    (rq : eevdf_runqueue)
    (thread_id generation weight slice_ns : Z)
  : eevdf_result :=
  if thread_id =? no_thread_id then fail EevdfErrInvalid rq
  else if negb (eevdf_valid_entity_params weight slice_ns)
  then fail EevdfErrInvalid rq
  else
    match find_entity_index rq thread_id with
    | Some _ => fail EevdfErrInvalid rq
    | None =>
        if (er_entity_count rq <? eevdf_max_entities)%nat then
          match eevdf_make_entity rq thread_id generation weight slice_ns with
          | Some entity => ok (refresh_runqueue (append_entity rq entity))
          | None => fail EevdfErrOverflow rq
          end
        else fail EevdfErrFull rq
    end.

Definition eevdf_wake
    (rq : eevdf_runqueue)
    (thread_id : Z)
  : eevdf_result :=
  match find_entity_index rq thread_id with
  | None => fail EevdfErrInvalid rq
  | Some index =>
      match lookup_entity rq index with
      | None => fail EevdfErrInvalid rq
      | Some entity =>
          match ee_state entity with
          | EBlocked =>
              match refresh_deadline
                (set_entity_state
                  (place_entity_at_floor entity (er_min_vruntime rq))
                  ERunnable)
                (er_min_vruntime rq)
              with
              | Some refreshed =>
                  ok (refresh_runqueue (replace_entity rq index refreshed))
              | None => fail EevdfErrOverflow rq
              end
          | _ => fail EevdfErrState rq
          end
      end
  end.

Definition eevdf_block
    (rq : eevdf_runqueue)
    (thread_id : Z)
  : eevdf_result :=
  match find_entity_index rq thread_id with
  | None => fail EevdfErrInvalid rq
  | Some index =>
      match lookup_entity rq index with
      | None => fail EevdfErrInvalid rq
      | Some entity =>
          if runnable_or_running entity then
            ok (refresh_runqueue
              (replace_entity rq index (set_entity_state entity EBlocked)))
          else fail EevdfErrState rq
      end
  end.

Definition eevdf_exit
    (rq : eevdf_runqueue)
    (thread_id : Z)
  : eevdf_result :=
  match find_entity_index rq thread_id with
  | None => fail EevdfErrInvalid rq
  | Some index =>
      match lookup_entity rq index with
      | None => fail EevdfErrInvalid rq
      | Some entity =>
          match ee_state entity with
          | EExited => fail EevdfErrState rq
          | EEmpty => fail EevdfErrState rq
          | _ =>
              ok (refresh_runqueue
                (replace_entity rq index (set_entity_state entity EExited)))
          end
      end
  end.

Definition eevdf_mark_running
    (rq : eevdf_runqueue)
    (thread_id : Z)
  : eevdf_result :=
  match find_entity_index rq thread_id with
  | None => fail EevdfErrInvalid rq
  | Some index =>
      match lookup_entity rq index with
      | None => fail EevdfErrInvalid rq
      | Some entity =>
          match ee_state entity with
          | ERunnable =>
              ok (recount_runqueue
                (replace_entity rq index (set_entity_state entity ERunning)))
          | _ => fail EevdfErrState rq
          end
      end
  end.

Definition eevdf_requeue_running
    (rq : eevdf_runqueue)
    (thread_id : Z)
  : eevdf_result :=
  match find_entity_index rq thread_id with
  | None => fail EevdfErrInvalid rq
  | Some index =>
      match lookup_entity rq index with
      | None => fail EevdfErrInvalid rq
      | Some entity =>
          match ee_state entity with
          | ERunning =>
              match refresh_deadline
                (set_entity_state entity ERunnable)
                (er_min_vruntime rq)
              with
              | Some refreshed =>
                  ok (refresh_runqueue (replace_entity rq index refreshed))
              | None => fail EevdfErrOverflow rq
              end
          | _ => fail EevdfErrState rq
          end
      end
  end.

Definition eevdf_charge_transition
    (rq : eevdf_runqueue)
    (thread_id runtime_ns : Z)
  : eevdf_result :=
  eevdf_charge rq thread_id runtime_ns.

Definition eevdf_pick_transition
    (rq : eevdf_runqueue)
  : pick_result :=
  eevdf_pick rq.

Theorem eevdf_reset_spec :
  forall rq,
    eevdf_reset rq = ok eevdf_empty_runqueue.
Proof.
  intros rq.
  reflexivity.
Qed.

Theorem eevdf_add_success_spec :
  forall rq thread_id generation weight slice_ns entity,
    thread_id <> no_thread_id ->
    eevdf_valid_entity_params weight slice_ns = true ->
    find_entity_index rq thread_id = None ->
    (er_entity_count rq < eevdf_max_entities)%nat ->
    eevdf_make_entity rq thread_id generation weight slice_ns = Some entity ->
    eevdf_add rq thread_id generation weight slice_ns =
      ok (refresh_runqueue (append_entity rq entity)).
Proof.
  intros rq thread_id generation weight slice_ns entity
    Hthread Hparams Hfind Hspace Hmake.
  unfold eevdf_add.
  destruct (thread_id =? no_thread_id) eqn:Hthread_eq.
  - apply Z.eqb_eq in Hthread_eq. contradiction.
  - rewrite Hparams.
    simpl.
    rewrite Hfind.
    apply Nat.ltb_lt in Hspace.
    rewrite Hspace.
    rewrite Hmake.
    reflexivity.
Qed.

Theorem eevdf_add_zero_thread_fails :
  forall rq generation weight slice_ns,
    eevdf_add rq no_thread_id generation weight slice_ns =
      fail EevdfErrInvalid rq.
Proof.
  intros rq generation weight slice_ns.
  unfold eevdf_add.
  rewrite Z.eqb_refl.
  reflexivity.
Qed.

Theorem eevdf_add_invalid_params_fails :
  forall rq thread_id generation weight slice_ns,
    thread_id <> no_thread_id ->
    eevdf_valid_entity_params weight slice_ns = false ->
    eevdf_add rq thread_id generation weight slice_ns =
      fail EevdfErrInvalid rq.
Proof.
  intros rq thread_id generation weight slice_ns Hthread Hparams.
  unfold eevdf_add.
  destruct (thread_id =? no_thread_id) eqn:Hthread_eq.
  - apply Z.eqb_eq in Hthread_eq. contradiction.
  - rewrite Hparams. reflexivity.
Qed.

Theorem eevdf_wake_success_spec :
  forall rq thread_id index entity refreshed,
    find_entity_index rq thread_id = Some index ->
    lookup_entity rq index = Some entity ->
    ee_state entity = EBlocked ->
    refresh_deadline
      (set_entity_state
        (place_entity_at_floor entity (er_min_vruntime rq))
        ERunnable)
      (er_min_vruntime rq) =
      Some refreshed ->
    eevdf_wake rq thread_id =
      ok (refresh_runqueue (replace_entity rq index refreshed)).
Proof.
  intros rq thread_id index entity refreshed Hfind Hlookup Hstate Hrefresh.
  unfold eevdf_wake.
  rewrite Hfind.
  rewrite Hlookup.
  rewrite Hstate.
  rewrite Hrefresh.
  reflexivity.
Qed.

Theorem eevdf_wake_success_places_at_floor :
  forall rq thread_id index entity refreshed rq',
    eevdf_wake rq thread_id = ok rq' ->
    find_entity_index rq thread_id = Some index ->
    lookup_entity rq index = Some entity ->
    ee_state entity = EBlocked ->
    refresh_deadline
      (set_entity_state
        (place_entity_at_floor entity (er_min_vruntime rq))
        ERunnable)
      (er_min_vruntime rq) =
      Some refreshed ->
    er_min_vruntime rq <= ee_vruntime refreshed /\
    ee_state refreshed = ERunnable /\
    rq' = refresh_runqueue (replace_entity rq index refreshed).
Proof.
  intros rq thread_id index entity refreshed rq'
    Hwake Hfind Hlookup Hstate Hrefresh.
  unfold eevdf_wake in Hwake.
  rewrite Hfind in Hwake.
  rewrite Hlookup in Hwake.
  rewrite Hstate in Hwake.
  rewrite Hrefresh in Hwake.
  inversion Hwake; subst rq'.
  pose proof (refresh_deadline_success
    (set_entity_state
      (place_entity_at_floor entity (er_min_vruntime rq))
      ERunnable)
    (er_min_vruntime rq)
    refreshed
    Hrefresh) as
    [_ [_ [_ [_ [_ [Hvruntime [Hstate_refreshed _]]]]]]].
  repeat split; auto.
  rewrite Hvruntime.
  simpl.
  apply z_max_ge_r.
Qed.

Theorem eevdf_block_success_spec :
  forall rq thread_id index entity,
    find_entity_index rq thread_id = Some index ->
    lookup_entity rq index = Some entity ->
    runnable_or_running entity = true ->
    eevdf_block rq thread_id =
      ok (refresh_runqueue
        (replace_entity rq index (set_entity_state entity EBlocked))).
Proof.
  intros rq thread_id index entity Hfind Hlookup Hstate.
  unfold eevdf_block.
  rewrite Hfind.
  rewrite Hlookup.
  rewrite Hstate.
  reflexivity.
Qed.

Theorem eevdf_block_success_formula :
  forall rq thread_id rq',
    eevdf_block rq thread_id = ok rq' ->
    exists index entity,
      find_entity_index rq thread_id = Some index /\
      lookup_entity rq index = Some entity /\
      runnable_or_running entity = true /\
      rq' =
        refresh_runqueue
          (replace_entity rq index (set_entity_state entity EBlocked)).
Proof.
  intros rq thread_id rq' Hblock.
  unfold eevdf_block in Hblock.
  destruct (find_entity_index rq thread_id) as [index |] eqn:Hfind;
    try discriminate.
  destruct (lookup_entity rq index) as [entity |] eqn:Hlookup;
    try discriminate.
  destruct (runnable_or_running entity) eqn:Hstate; try discriminate.
  inversion Hblock; subst rq'.
  repeat eexists; repeat split; eauto.
Qed.

Theorem eevdf_exit_success_spec :
  forall rq thread_id index entity,
    find_entity_index rq thread_id = Some index ->
    lookup_entity rq index = Some entity ->
    ee_state entity <> EEmpty ->
    ee_state entity <> EExited ->
    eevdf_exit rq thread_id =
      ok (refresh_runqueue
        (replace_entity rq index (set_entity_state entity EExited))).
Proof.
  intros rq thread_id index entity Hfind Hlookup Hnot_empty Hnot_exited.
  unfold eevdf_exit.
  rewrite Hfind.
  rewrite Hlookup.
  destruct (ee_state entity); try reflexivity; contradiction.
Qed.

Theorem eevdf_exit_success_formula :
  forall rq thread_id rq',
    eevdf_exit rq thread_id = ok rq' ->
    exists index entity,
      find_entity_index rq thread_id = Some index /\
      lookup_entity rq index = Some entity /\
      ee_state entity <> EEmpty /\
      ee_state entity <> EExited /\
      rq' =
        refresh_runqueue
          (replace_entity rq index (set_entity_state entity EExited)).
Proof.
  intros rq thread_id rq' Hexit.
  unfold eevdf_exit in Hexit.
  destruct (find_entity_index rq thread_id) as [index |] eqn:Hfind;
    try discriminate.
  destruct (lookup_entity rq index) as [entity |] eqn:Hlookup;
    try discriminate.
  destruct (ee_state entity) eqn:Hstate; try discriminate;
    inversion Hexit; subst rq';
    repeat eexists; repeat split; eauto; congruence.
Qed.

Theorem eevdf_mark_running_success_spec :
  forall rq thread_id index entity,
    find_entity_index rq thread_id = Some index ->
    lookup_entity rq index = Some entity ->
    ee_state entity = ERunnable ->
    eevdf_mark_running rq thread_id =
      ok (recount_runqueue
        (replace_entity rq index (set_entity_state entity ERunning))).
Proof.
  intros rq thread_id index entity Hfind Hlookup Hstate.
  unfold eevdf_mark_running.
  rewrite Hfind.
  rewrite Hlookup.
  rewrite Hstate.
  reflexivity.
Qed.

Theorem eevdf_mark_running_success_formula :
  forall rq thread_id rq',
    eevdf_mark_running rq thread_id = ok rq' ->
    exists index entity,
      find_entity_index rq thread_id = Some index /\
      lookup_entity rq index = Some entity /\
      ee_state entity = ERunnable /\
      rq' =
        recount_runqueue
          (replace_entity rq index (set_entity_state entity ERunning)).
Proof.
  intros rq thread_id rq' Hmark.
  unfold eevdf_mark_running in Hmark.
  destruct (find_entity_index rq thread_id) as [index |] eqn:Hfind;
    try discriminate.
  destruct (lookup_entity rq index) as [entity |] eqn:Hlookup;
    try discriminate.
  destruct (ee_state entity) eqn:Hstate; try discriminate;
    inversion Hmark; subst rq';
    repeat eexists; repeat split; eauto.
Qed.

Theorem eevdf_requeue_running_success_spec :
  forall rq thread_id index entity refreshed,
    find_entity_index rq thread_id = Some index ->
    lookup_entity rq index = Some entity ->
    ee_state entity = ERunning ->
    refresh_deadline (set_entity_state entity ERunnable) (er_min_vruntime rq) =
      Some refreshed ->
    eevdf_requeue_running rq thread_id =
      ok (refresh_runqueue (replace_entity rq index refreshed)).
Proof.
  intros rq thread_id index entity refreshed Hfind Hlookup Hstate Hrefresh.
  unfold eevdf_requeue_running.
  rewrite Hfind.
  rewrite Hlookup.
  rewrite Hstate.
  rewrite Hrefresh.
  reflexivity.
Qed.

Theorem eevdf_requeue_running_success_formula :
  forall rq thread_id rq',
    eevdf_requeue_running rq thread_id = ok rq' ->
    exists index entity refreshed,
      find_entity_index rq thread_id = Some index /\
      lookup_entity rq index = Some entity /\
      ee_state entity = ERunning /\
      refresh_deadline (set_entity_state entity ERunnable) (er_min_vruntime rq) =
        Some refreshed /\
      rq' = refresh_runqueue (replace_entity rq index refreshed).
Proof.
  intros rq thread_id rq' Hrequeue.
  unfold eevdf_requeue_running in Hrequeue.
  destruct (find_entity_index rq thread_id) as [index |] eqn:Hfind;
    try discriminate.
  destruct (lookup_entity rq index) as [entity |] eqn:Hlookup;
    try discriminate.
  destruct (ee_state entity) eqn:Hstate; try discriminate.
  destruct (refresh_deadline (set_entity_state entity ERunnable) (er_min_vruntime rq))
    as [refreshed |] eqn:Hrefresh; try discriminate.
  inversion Hrequeue; subst rq'.
  repeat eexists; repeat split; eauto.
Qed.

Theorem eevdf_charge_transition_spec :
  forall rq thread_id runtime_ns,
    eevdf_charge_transition rq thread_id runtime_ns =
      eevdf_charge rq thread_id runtime_ns.
Proof.
  reflexivity.
Qed.

Theorem eevdf_pick_transition_spec :
  forall rq,
    eevdf_pick_transition rq = eevdf_pick rq.
Proof.
  reflexivity.
Qed.
