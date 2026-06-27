From Stdlib Require Import Bool.Bool Lia Lists.List ZArith.ZArith.
From Pacha.Scheduling Require Import ProtocolModel EevdfModel EevdfInvariants.

Open Scope Z_scope.

Theorem weighted_delta_weight_zero_fails :
  forall runtime_ns,
    weighted_delta runtime_ns 0 = None.
Proof.
  intros runtime_ns.
  unfold weighted_delta, valid_positive.
  simpl.
  destruct (i64_nonnegative runtime_ns); reflexivity.
Qed.

Theorem eevdf_charge_success_formula :
  forall rq thread_id runtime_ns rq',
    eevdf_charge rq thread_id runtime_ns =
      {|
        eevdf_result_rc := EevdfOk;
        eevdf_result_rq := rq';
      |} ->
    exists index old_entity charged_entity refreshed delta,
      find_entity_index rq thread_id = Some index /\
      lookup_entity rq index = Some old_entity /\
      runnable_or_running old_entity = true /\
      weighted_delta runtime_ns (ee_weight old_entity) = Some delta /\
      i64_value (ee_vruntime old_entity + delta) = true /\
      ee_thread_id charged_entity = ee_thread_id old_entity /\
      ee_generation charged_entity = ee_generation old_entity /\
      ee_weight charged_entity = ee_weight old_entity /\
      ee_slice_ns charged_entity = ee_slice_ns old_entity /\
      ee_state charged_entity = ee_state old_entity /\
      ee_vruntime charged_entity = ee_vruntime old_entity + delta /\
      ee_service_ns charged_entity = ee_service_ns old_entity + runtime_ns /\
      refresh_deadline charged_entity (er_min_vruntime rq) = Some refreshed /\
      rq' = refresh_runqueue (replace_entity rq index refreshed).
Proof.
  intros rq thread_id runtime_ns rq' Hcharge.
  unfold eevdf_charge in Hcharge.
  destruct (find_entity_index rq thread_id) as [index |] eqn:Hfind;
    try discriminate.
  destruct (lookup_entity rq index) as [old_entity |] eqn:Hlookup;
    try discriminate.
  destruct old_entity as
    [old_thread_id old_generation old_weight old_slice_ns old_service_ns
      old_vruntime old_eligible old_deadline old_state].
  simpl in *.
  destruct old_state; try discriminate.
  - destruct (weighted_delta runtime_ns old_weight) as [delta |] eqn:Hdelta;
      try discriminate.
    destruct (i64_value (old_vruntime + delta)) eqn:Hrange;
      try discriminate.
    destruct (refresh_deadline
      {|
        ee_thread_id := old_thread_id;
        ee_generation := old_generation;
        ee_weight := old_weight;
        ee_slice_ns := old_slice_ns;
        ee_service_ns := old_service_ns + runtime_ns;
        ee_vruntime := old_vruntime + delta;
        ee_eligible_time := old_eligible;
        ee_deadline := old_deadline;
        ee_state := ERunnable;
      |}
      (er_min_vruntime rq)) as [refreshed |] eqn:Hrefresh;
      try discriminate.
    inversion Hcharge; subst.
    exists index.
    exists {|
      ee_thread_id := old_thread_id;
      ee_generation := old_generation;
      ee_weight := old_weight;
      ee_slice_ns := old_slice_ns;
      ee_service_ns := old_service_ns;
      ee_vruntime := old_vruntime;
      ee_eligible_time := old_eligible;
      ee_deadline := old_deadline;
      ee_state := ERunnable;
    |}.
    exists {|
      ee_thread_id := old_thread_id;
      ee_generation := old_generation;
      ee_weight := old_weight;
      ee_slice_ns := old_slice_ns;
      ee_service_ns := old_service_ns + runtime_ns;
      ee_vruntime := old_vruntime + delta;
      ee_eligible_time := old_eligible;
      ee_deadline := old_deadline;
      ee_state := ERunnable;
    |}.
    exists refreshed.
    exists delta.
    repeat split; simpl; auto; try reflexivity.
  - destruct (weighted_delta runtime_ns old_weight) as [delta |] eqn:Hdelta;
      try discriminate.
    destruct (i64_value (old_vruntime + delta)) eqn:Hrange;
      try discriminate.
    destruct (refresh_deadline
      {|
        ee_thread_id := old_thread_id;
        ee_generation := old_generation;
        ee_weight := old_weight;
        ee_slice_ns := old_slice_ns;
        ee_service_ns := old_service_ns + runtime_ns;
        ee_vruntime := old_vruntime + delta;
        ee_eligible_time := old_eligible;
        ee_deadline := old_deadline;
        ee_state := ERunning;
      |}
      (er_min_vruntime rq)) as [refreshed |] eqn:Hrefresh;
      try discriminate.
    inversion Hcharge; subst.
    exists index.
    exists {|
      ee_thread_id := old_thread_id;
      ee_generation := old_generation;
      ee_weight := old_weight;
      ee_slice_ns := old_slice_ns;
      ee_service_ns := old_service_ns;
      ee_vruntime := old_vruntime;
      ee_eligible_time := old_eligible;
      ee_deadline := old_deadline;
      ee_state := ERunning;
    |}.
    exists {|
      ee_thread_id := old_thread_id;
      ee_generation := old_generation;
      ee_weight := old_weight;
      ee_slice_ns := old_slice_ns;
      ee_service_ns := old_service_ns + runtime_ns;
      ee_vruntime := old_vruntime + delta;
      ee_eligible_time := old_eligible;
      ee_deadline := old_deadline;
      ee_state := ERunning;
    |}.
    exists refreshed.
    exists delta.
    repeat split; simpl; auto; try reflexivity.
Qed.

Lemma charge_nth_error_replace_nth_eq :
  forall {A : Type} (items : list A) index item,
    (index < length items)%nat ->
    nth_error (replace_nth items index item) index = Some item.
Proof.
  induction items as [| head rest IH]; intros index item Hlt.
  - simpl in Hlt. lia.
  - destruct index as [| index']; simpl in *.
    + reflexivity.
    + apply IH.
      lia.
Qed.

Lemma charge_find_entity_index_from_some_match :
  forall entities remaining thread_id base index,
    find_entity_index_from entities remaining thread_id base = Some index ->
    exists offset entity,
      index = (base + offset)%nat /\
      nth_error entities offset = Some entity /\
      (offset < remaining)%nat /\
      ee_thread_id entity = thread_id /\
      is_active_state (ee_state entity) = true.
Proof.
  induction entities as [| head rest IH];
    intros remaining thread_id base index Hfind;
    destruct remaining as [| remaining']; simpl in Hfind; try discriminate.
  destruct
    (andb
      (ee_thread_id head =? thread_id)
      (is_active_state (ee_state head))) eqn:Hmatch.
  - apply andb_true_iff in Hmatch as [Hthread Hactive].
    apply Z.eqb_eq in Hthread.
    inversion Hfind; subst index.
    exists 0%nat, head.
    repeat split; auto; lia.
  - destruct (IH remaining' thread_id (S base) index Hfind)
      as [offset [entity [Hindex [Hlookup [Hlt [Hthread Hactive]]]]]].
    exists (S offset), entity.
    repeat split; auto; lia.
Qed.

Lemma charge_find_entity_index_lookup_matches_thread :
  forall rq thread_id index entity,
    find_entity_index rq thread_id = Some index ->
    lookup_entity rq index = Some entity ->
    ee_thread_id entity = thread_id /\
    is_active_state (ee_state entity) = true.
Proof.
  intros rq thread_id index entity Hfind Hlookup.
  unfold find_entity_index in Hfind.
  destruct (thread_id =? no_thread_id); try discriminate.
  destruct (charge_find_entity_index_from_some_match
    (er_entities rq)
    (er_entity_count rq)
    thread_id
    0%nat
    index
    Hfind) as [offset [matched [Hindex [Hmatched_lookup [_ [Hthread Hactive]]]]]].
  simpl in Hindex.
  subst index.
  unfold lookup_entity in Hlookup.
  rewrite Hmatched_lookup in Hlookup.
  inversion Hlookup; subst matched.
  split; auto.
Qed.

Lemma charge_find_entity_index_lt :
  forall rq thread_id index,
    find_entity_index rq thread_id = Some index ->
    (index < er_entity_count rq)%nat.
Proof.
  intros rq thread_id index Hfind.
  unfold find_entity_index in Hfind.
  destruct (thread_id =? no_thread_id); try discriminate.
  destruct (charge_find_entity_index_from_some_match
    (er_entities rq)
    (er_entity_count rq)
    thread_id
    0%nat
    index
    Hfind) as [offset [_entity [Hindex [_lookup [Hlt _rest]]]]].
  simpl in Hindex.
  subst index.
  exact Hlt.
Qed.

Theorem eevdf_charge_success_has_charged_entity :
  forall rq thread_id runtime_ns rq',
    eevdf_charge rq thread_id runtime_ns = ok rq' ->
    exists index old_entity refreshed,
      eevdf_active_entity rq' index refreshed /\
      ee_thread_id old_entity = thread_id /\
      ee_thread_id refreshed = thread_id /\
      ee_generation refreshed = ee_generation old_entity /\
      ee_state refreshed = ee_state old_entity /\
      ee_service_ns refreshed = ee_service_ns old_entity + runtime_ns.
Proof.
  intros rq thread_id runtime_ns rq' Hcharge.
  destruct (eevdf_charge_success_formula rq thread_id runtime_ns rq' Hcharge)
    as [index [old_entity [charged_entity [refreshed [delta Hsuccess]]]]].
  destruct Hsuccess as
    [Hfind
    [Hlookup
    [_Hstate
    [_Hdelta
    [_Hrange
    [Hcharged_thread
    [Hcharged_generation
    [_Hcharged_weight
    [_Hcharged_slice
    [Hcharged_state
    [_Hcharged_vruntime
    [Hcharged_service
    [Hrefresh Hrq']]]]]]]]]]]]].
  pose proof (charge_find_entity_index_lookup_matches_thread
    rq
    thread_id
    index
    old_entity
    Hfind
    Hlookup) as [Hold_thread _Hold_active].
  pose proof (refresh_deadline_success
    charged_entity
    (er_min_vruntime rq)
    refreshed
    Hrefresh) as
    [Hrefreshed_thread
    [Hrefreshed_generation
    [_Hrefreshed_weight
    [_Hrefreshed_slice
    [Hrefreshed_service
    [_Hrefreshed_vruntime
    [Hrefreshed_state _Hrefreshed_deadline]]]]]]].
  subst rq'.
  exists index, old_entity, refreshed.
  split.
  - unfold eevdf_active_entity, refresh_runqueue, replace_entity.
    simpl.
    split.
    + apply charge_find_entity_index_lt with (thread_id := thread_id).
      exact Hfind.
    + apply charge_nth_error_replace_nth_eq.
      unfold lookup_entity in Hlookup.
      apply nth_error_Some.
      rewrite Hlookup.
      discriminate.
  - repeat split.
    + exact Hold_thread.
    + rewrite Hrefreshed_thread.
      rewrite Hcharged_thread.
      exact Hold_thread.
    + rewrite Hrefreshed_generation.
      exact Hcharged_generation.
    + rewrite Hrefreshed_state.
      exact Hcharged_state.
    + rewrite Hrefreshed_service.
      exact Hcharged_service.
Qed.

Theorem eevdf_charge_success_has_matching_running_entity :
  forall rq thread_id runtime_ns rq' source_index source_entity,
    eevdf_invariant rq ->
    eevdf_active_entity rq source_index source_entity ->
    ee_state source_entity = ERunning ->
    ee_thread_id source_entity = thread_id ->
    eevdf_charge rq thread_id runtime_ns = ok rq' ->
    exists charged_index charged_entity,
      eevdf_active_entity rq' charged_index charged_entity /\
      ee_state charged_entity = ERunning /\
      ee_thread_id charged_entity = thread_id /\
      ee_generation charged_entity = ee_generation source_entity.
Proof.
  intros rq thread_id runtime_ns rq' source_index source_entity
    Hinv Hsource_active Hsource_state Hsource_thread Hcharge.
  destruct (eevdf_charge_success_formula rq thread_id runtime_ns rq' Hcharge)
    as [index [old_entity [charged_entity [refreshed [delta Hsuccess]]]]].
  destruct Hsuccess as
    [Hfind
    [Hlookup
    [_Hstate
    [_Hdelta
    [_Hrange
    [Hcharged_thread
    [Hcharged_generation
    [_Hcharged_weight
    [_Hcharged_slice
    [Hcharged_state
    [_Hcharged_vruntime
    [_Hcharged_service
    [Hrefresh Hrq']]]]]]]]]]]]].
  pose proof (charge_find_entity_index_lookup_matches_thread
    rq
    thread_id
    index
    old_entity
    Hfind
    Hlookup) as [Hold_thread Hold_active_state].
  assert (Hold_active : eevdf_active_entity rq index old_entity).
  {
    split.
    - apply charge_find_entity_index_lt with (thread_id := thread_id).
      exact Hfind.
    - unfold lookup_entity in Hlookup.
      exact Hlookup.
  }
  destruct Hinv as
    [_Hshape
    [_Hinactive
    [Hunique
    [_Hpositive
    [_Hlive
    [_Hmin
    [_Hvirtual _Hrunnable]]]]]]].
  assert (Hthread_not_none : ee_thread_id old_entity <> no_thread_id).
  {
    rewrite Hold_thread.
    unfold find_entity_index in Hfind.
    destruct (thread_id =? no_thread_id) eqn:Hno; try discriminate.
    apply Z.eqb_neq in Hno.
    exact Hno.
  }
  assert (Hsame_index : index = source_index).
  {
    apply Hunique with (lhs := old_entity) (rhs := source_entity).
    - exact Hold_active.
    - exact Hsource_active.
    - exact Hthread_not_none.
    - rewrite Hold_thread.
      symmetry.
      exact Hsource_thread.
    - exact Hold_active_state.
    - rewrite Hsource_state.
      reflexivity.
  }
  subst source_index.
  destruct Hold_active as [_ Hold_lookup].
  destruct Hsource_active as [_ Hsource_lookup].
  rewrite Hold_lookup in Hsource_lookup.
  inversion Hsource_lookup; subst old_entity.
  pose proof (refresh_deadline_success
    charged_entity
    (er_min_vruntime rq)
    refreshed
    Hrefresh) as
    [Hrefreshed_thread
    [Hrefreshed_generation
    [_Hrefreshed_weight
    [_Hrefreshed_slice
    [_Hrefreshed_service
    [_Hrefreshed_vruntime
    [Hrefreshed_state _Hrefreshed_deadline]]]]]]].
  subst rq'.
  exists index, refreshed.
  split.
  - unfold eevdf_active_entity, refresh_runqueue, replace_entity.
    simpl.
    split.
    + apply charge_find_entity_index_lt with (thread_id := thread_id).
      exact Hfind.
    + apply charge_nth_error_replace_nth_eq.
      unfold lookup_entity in Hlookup.
      apply nth_error_Some.
      rewrite Hlookup.
      discriminate.
  - split.
    + rewrite Hrefreshed_state.
      rewrite Hcharged_state.
      exact Hsource_state.
    + split.
      * rewrite Hrefreshed_thread.
        rewrite Hcharged_thread.
        exact Hsource_thread.
      * rewrite Hrefreshed_generation.
        exact Hcharged_generation.
Qed.

Theorem eevdf_charge_weight_zero_fails :
  forall rq thread_id runtime_ns index entity,
    find_entity_index rq thread_id = Some index ->
    lookup_entity rq index = Some entity ->
    runnable_or_running entity = true ->
    ee_weight entity = 0 ->
    eevdf_charge rq thread_id runtime_ns = fail EevdfErrOverflow rq.
Proof.
  intros rq thread_id runtime_ns index entity Hfind Hlookup Hstate Hweight.
  unfold eevdf_charge.
  rewrite Hfind.
  rewrite Hlookup.
  destruct entity as
    [old_thread_id old_generation old_weight old_slice_ns old_service_ns
      old_vruntime old_eligible old_deadline old_state].
  simpl in *.
  subst old_weight.
  destruct old_state; try discriminate; simpl in *;
    rewrite weighted_delta_weight_zero_fails; reflexivity.
Qed.

Theorem eevdf_charge_delta_overflow_fails :
  forall rq thread_id runtime_ns index entity,
    find_entity_index rq thread_id = Some index ->
    lookup_entity rq index = Some entity ->
    runnable_or_running entity = true ->
    weighted_delta runtime_ns (ee_weight entity) = None ->
    eevdf_charge rq thread_id runtime_ns = fail EevdfErrOverflow rq.
Proof.
  intros rq thread_id runtime_ns index entity Hfind Hlookup Hstate Hdelta.
  unfold eevdf_charge.
  rewrite Hfind.
  rewrite Hlookup.
  destruct entity as
    [old_thread_id old_generation old_weight old_slice_ns old_service_ns
      old_vruntime old_eligible old_deadline old_state].
  simpl in *.
  destruct old_state; try discriminate; simpl in *; rewrite Hdelta; reflexivity.
Qed.

Theorem eevdf_charge_vruntime_overflow_fails :
  forall rq thread_id runtime_ns index entity delta,
    find_entity_index rq thread_id = Some index ->
    lookup_entity rq index = Some entity ->
    runnable_or_running entity = true ->
    weighted_delta runtime_ns (ee_weight entity) = Some delta ->
    i64_value (ee_vruntime entity + delta) = false ->
    eevdf_charge rq thread_id runtime_ns = fail EevdfErrOverflow rq.
Proof.
  intros rq thread_id runtime_ns index entity delta
    Hfind Hlookup Hstate Hdelta Hrange.
  unfold eevdf_charge.
  rewrite Hfind.
  rewrite Hlookup.
  destruct entity as
    [old_thread_id old_generation old_weight old_slice_ns old_service_ns
      old_vruntime old_eligible old_deadline old_state].
  simpl in *.
  destruct old_state; try discriminate; simpl in *;
    rewrite Hdelta; rewrite Hrange; reflexivity.
Qed.
