From Stdlib Require Import Bool.Bool Lia Lists.List ZArith.ZArith.
From Pacha.Scheduling Require Import ProtocolModel EevdfModel.

Import ListNotations.
Open Scope Z_scope.

Definition eevdf_active_entity
    (rq : eevdf_runqueue)
    (index : nat)
    (entity : eevdf_entity)
  : Prop :=
  (index < er_entity_count rq)%nat /\
  nth_error (er_entities rq) index = Some entity.

Definition eevdf_shape
    (rq : eevdf_runqueue)
  : Prop :=
  (er_entity_count rq <= eevdf_max_entities)%nat /\
  length (er_entities rq) = eevdf_max_entities.

Definition eevdf_inactive_entities_empty
    (rq : eevdf_runqueue)
  : Prop :=
  forall index entity,
    nth_error (er_entities rq) index = Some entity ->
    (er_entity_count rq <= index)%nat ->
    ee_state entity = EEmpty.

Definition eevdf_active_thread_ids_unique
    (rq : eevdf_runqueue)
  : Prop :=
  forall i j lhs rhs,
    eevdf_active_entity rq i lhs ->
    eevdf_active_entity rq j rhs ->
    ee_thread_id lhs <> no_thread_id ->
    ee_thread_id lhs = ee_thread_id rhs ->
    is_active_state (ee_state lhs) = true ->
    is_active_state (ee_state rhs) = true ->
    i = j.

Definition eevdf_positive_entity_params
    (rq : eevdf_runqueue)
  : Prop :=
  forall index entity,
    eevdf_active_entity rq index entity ->
    is_active_state (ee_state entity) = true ->
    0 < ee_weight entity /\
    0 < ee_slice_ns entity.

Definition eevdf_runnable_deadlines_consistent
    (rq : eevdf_runqueue)
  : Prop :=
  forall index entity,
    eevdf_active_entity rq index entity ->
    ee_state entity = ERunnable ->
    exists slice,
      weighted_slice (ee_slice_ns entity) (ee_weight entity) = Some slice /\
      ee_eligible_time entity = z_max (ee_vruntime entity) (er_min_vruntime rq) /\
      er_min_vruntime rq <= ee_eligible_time entity /\
      ee_deadline entity = ee_eligible_time entity + slice.

Definition eevdf_live_deadlines_consistent
    (rq : eevdf_runqueue)
  : Prop :=
  forall index entity,
    eevdf_active_entity rq index entity ->
    live_for_min entity = true ->
    exists slice,
      weighted_slice (ee_slice_ns entity) (ee_weight entity) = Some slice /\
      ee_eligible_time entity = z_max (ee_vruntime entity) (er_min_vruntime rq) /\
      er_min_vruntime rq <= ee_eligible_time entity /\
      ee_deadline entity = ee_eligible_time entity + slice.

Definition eevdf_min_vruntime_lower_bound
    (rq : eevdf_runqueue)
  : Prop :=
  forall index entity,
    eevdf_active_entity rq index entity ->
    live_for_min entity = true ->
    er_min_vruntime rq <= ee_vruntime entity.

Definition eevdf_min_vruntime_attained
    (rq : eevdf_runqueue)
  : Prop :=
  forall index entity,
    eevdf_active_entity rq index entity ->
    live_for_min entity = true ->
    exists min_index min_entity,
      eevdf_active_entity rq min_index min_entity /\
      live_for_min min_entity = true /\
      ee_vruntime min_entity = er_min_vruntime rq.

Definition eevdf_min_vruntime_consistent
    (rq : eevdf_runqueue)
  : Prop :=
  eevdf_min_vruntime_lower_bound rq /\
  eevdf_min_vruntime_attained rq.

Definition eevdf_virtual_time_consistent
    (rq : eevdf_runqueue)
  : Prop :=
  er_min_vruntime rq <= er_virtual_time rq.

Definition eevdf_runnable_count_consistent
    (rq : eevdf_runqueue)
  : Prop :=
  er_runnable_count rq =
    count_runnable (firstn (er_entity_count rq) (er_entities rq)).

Definition eevdf_invariant
    (rq : eevdf_runqueue)
  : Prop :=
  eevdf_shape rq /\
  eevdf_inactive_entities_empty rq /\
  eevdf_active_thread_ids_unique rq /\
  eevdf_positive_entity_params rq /\
  eevdf_live_deadlines_consistent rq /\
  eevdf_min_vruntime_consistent rq /\
  eevdf_virtual_time_consistent rq /\
  eevdf_runnable_count_consistent rq.

Lemma runnable_entity_live :
  forall entity,
    ee_state entity = ERunnable ->
    live_for_min entity = true.
Proof.
  intros entity Hstate.
  unfold live_for_min.
  rewrite Hstate.
  reflexivity.
Qed.

Lemma running_entity_live :
  forall entity,
    ee_state entity = ERunning ->
    live_for_min entity = true.
Proof.
  intros entity Hstate.
  unfold live_for_min.
  rewrite Hstate.
  reflexivity.
Qed.

Theorem eevdf_live_deadlines_imply_runnable_deadlines :
  forall rq,
    eevdf_live_deadlines_consistent rq ->
    eevdf_runnable_deadlines_consistent rq.
Proof.
  intros rq Hlive index entity Hactive Hrunnable.
  apply (Hlive index entity); auto.
  apply runnable_entity_live; exact Hrunnable.
Qed.

Theorem eevdf_invariant_runnable_deadlines_consistent :
  forall rq,
    eevdf_invariant rq ->
    eevdf_runnable_deadlines_consistent rq.
Proof.
  intros rq [_ [_ [_ [_ [Hlive _]]]]].
  apply eevdf_live_deadlines_imply_runnable_deadlines.
  exact Hlive.
Qed.

Theorem eevdf_invariant_running_deadline_consistent :
  forall rq index entity,
    eevdf_invariant rq ->
    eevdf_active_entity rq index entity ->
    ee_state entity = ERunning ->
    exists slice,
      weighted_slice (ee_slice_ns entity) (ee_weight entity) = Some slice /\
      ee_eligible_time entity = z_max (ee_vruntime entity) (er_min_vruntime rq) /\
      er_min_vruntime rq <= ee_eligible_time entity /\
      ee_deadline entity = ee_eligible_time entity + slice.
Proof.
  intros rq index entity [_ [_ [_ [_ [Hlive _]]]]] Hactive Hrunning.
  apply (Hlive index entity); auto.
  apply running_entity_live; exact Hrunning.
Qed.

Lemma z_max_ge_l :
  forall lhs rhs,
    lhs <= z_max lhs rhs.
Proof.
  intros lhs rhs.
  unfold z_max.
  destruct (lhs <? rhs) eqn:Hlt.
  - apply Z.ltb_lt in Hlt. lia.
  - lia.
Qed.

Lemma z_max_ge_r :
  forall lhs rhs,
    rhs <= z_max lhs rhs.
Proof.
  intros lhs rhs.
  unfold z_max.
  destruct (lhs <? rhs) eqn:Hlt.
  - lia.
  - apply Z.ltb_ge in Hlt. lia.
Qed.

Lemma weighted_slice_positive :
  forall slice_ns weight slice,
    weighted_slice slice_ns weight = Some slice ->
    0 < slice.
Proof.
  intros slice_ns weight slice Hslice.
  unfold weighted_slice in Hslice.
  destruct (weighted_delta slice_ns weight) as [delta |] eqn:Hdelta;
    try discriminate.
  inversion Hslice; subst.
  unfold z_max.
  destruct (1 <? delta) eqn:Hlt.
  - apply Z.ltb_lt in Hlt. lia.
  - lia.
Qed.

Lemma refresh_deadline_success :
  forall entity floor refreshed,
    refresh_deadline entity floor = Some refreshed ->
    ee_thread_id refreshed = ee_thread_id entity /\
    ee_generation refreshed = ee_generation entity /\
    ee_weight refreshed = ee_weight entity /\
    ee_slice_ns refreshed = ee_slice_ns entity /\
    ee_service_ns refreshed = ee_service_ns entity /\
    ee_vruntime refreshed = ee_vruntime entity /\
    ee_state refreshed = ee_state entity /\
    exists slice,
      weighted_slice (ee_slice_ns entity) (ee_weight entity) = Some slice /\
      ee_eligible_time refreshed = z_max (ee_vruntime entity) floor /\
      floor <= ee_eligible_time refreshed /\
      ee_deadline refreshed = ee_eligible_time refreshed + slice.
Proof.
  intros entity floor refreshed Hrefresh.
  unfold refresh_deadline in Hrefresh.
  destruct (weighted_slice (ee_slice_ns entity) (ee_weight entity))
    as [slice |] eqn:Hslice; try discriminate.
  destruct (i64_value (z_max (ee_vruntime entity) floor + slice)) eqn:Hrange;
    try discriminate.
  inversion Hrefresh; subst.
  repeat split; auto.
  exists slice.
  repeat split; auto.
  apply z_max_ge_r.
Qed.

Theorem refresh_deadline_runnable_consistent :
  forall rq index entity refreshed,
    refresh_deadline entity (er_min_vruntime rq) = Some refreshed ->
    ee_state refreshed = ERunnable ->
    eevdf_active_entity
      {|
        er_entities := replace_nth (er_entities rq) index refreshed;
        er_entity_count := er_entity_count rq;
        er_runnable_count := er_runnable_count rq;
        er_virtual_time := er_virtual_time rq;
        er_min_vruntime := er_min_vruntime rq;
      |}
      index
      refreshed ->
    exists slice,
      weighted_slice (ee_slice_ns refreshed) (ee_weight refreshed) = Some slice /\
      ee_eligible_time refreshed =
        z_max (ee_vruntime refreshed) (er_min_vruntime rq) /\
      er_min_vruntime rq <= ee_eligible_time refreshed /\
      ee_deadline refreshed = ee_eligible_time refreshed + slice.
Proof.
  intros rq index entity refreshed Hrefresh _ _.
  pose proof (refresh_deadline_success entity (er_min_vruntime rq) refreshed Hrefresh)
    as [_ [_ [Hweight [Hslice_ns [_ [Hvruntime [_ Hdeadline]]]]]]].
  destruct Hdeadline as [slice [Hslice [Heligible [Hfloor Hdeadline]]]].
  exists slice.
  repeat split; auto;
    try (rewrite Hslice_ns; rewrite Hweight; exact Hslice);
    try (rewrite Hvruntime; exact Heligible).
Qed.

Theorem refresh_deadline_live_consistent :
  forall rq index entity refreshed,
    refresh_deadline entity (er_min_vruntime rq) = Some refreshed ->
    live_for_min refreshed = true ->
    eevdf_active_entity
      {|
        er_entities := replace_nth (er_entities rq) index refreshed;
        er_entity_count := er_entity_count rq;
        er_runnable_count := er_runnable_count rq;
        er_virtual_time := er_virtual_time rq;
        er_min_vruntime := er_min_vruntime rq;
      |}
      index
      refreshed ->
    exists slice,
      weighted_slice (ee_slice_ns refreshed) (ee_weight refreshed) = Some slice /\
      ee_eligible_time refreshed =
        z_max (ee_vruntime refreshed) (er_min_vruntime rq) /\
      er_min_vruntime rq <= ee_eligible_time refreshed /\
      ee_deadline refreshed = ee_eligible_time refreshed + slice.
Proof.
  intros rq index entity refreshed Hrefresh _ _.
  pose proof (refresh_deadline_success entity (er_min_vruntime rq) refreshed Hrefresh)
    as [_ [_ [Hweight [Hslice_ns [_ [Hvruntime [_ Hdeadline]]]]]]].
  destruct Hdeadline as [slice [Hslice [Heligible [Hfloor Hdeadline]]]].
  exists slice.
  repeat split; auto;
    try (rewrite Hslice_ns; rewrite Hweight; exact Hslice);
    try (rewrite Hvruntime; exact Heligible).
Qed.

Lemma place_entity_at_floor_vruntime_ge :
  forall entity floor,
    floor <= ee_vruntime (place_entity_at_floor entity floor).
Proof.
  intros entity floor.
  unfold place_entity_at_floor.
  simpl.
  apply z_max_ge_r.
Qed.

Lemma place_entity_at_floor_state :
  forall entity floor,
    ee_state (place_entity_at_floor entity floor) = ee_state entity.
Proof.
  intros entity floor.
  reflexivity.
Qed.
