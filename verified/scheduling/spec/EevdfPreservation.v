From Stdlib Require Import Arith.PeanoNat Bool.Bool Lia Lists.List ZArith.ZArith.
From Pacha.Scheduling Require Import
  ProtocolModel
  EevdfModel
  EevdfInvariants
  EevdfPick
  EevdfCharge
  EevdfTransitions.

Import ListNotations.
Open Scope Z_scope.

Definition result_preserves_invariant
    (result : eevdf_result)
  : Prop :=
  eevdf_result_rc result = EevdfOk ->
  eevdf_invariant (eevdf_result_rq result).

Lemma eevdf_invariant_intro :
  forall rq,
    eevdf_shape rq ->
    eevdf_inactive_entities_empty rq ->
    eevdf_active_thread_ids_unique rq ->
    eevdf_positive_entity_params rq ->
    eevdf_live_deadlines_consistent rq ->
    eevdf_min_vruntime_consistent rq ->
    eevdf_virtual_time_consistent rq ->
    eevdf_runnable_count_consistent rq ->
    eevdf_invariant rq.
Proof.
  intros rq Hshape Hinactive Hunique Hpositive Hlive Hmin Hvirtual Hrunnable.
  unfold eevdf_invariant.
  split; [exact Hshape |].
  split; [exact Hinactive |].
  split; [exact Hunique |].
  split; [exact Hpositive |].
  split; [exact Hlive |].
  split; [exact Hmin |].
  split; [exact Hvirtual | exact Hrunnable].
Qed.

Lemma repeat_empty_entity_state :
  forall index entity,
    nth_error (repeat eevdf_empty_entity eevdf_max_entities) index =
      Some entity ->
    ee_state entity = EEmpty.
Proof.
  intros index entity Hlookup.
  apply nth_error_In in Hlookup.
  apply repeat_spec in Hlookup.
  subst entity.
  reflexivity.
Qed.

Lemma replace_nth_length :
  forall {A : Type} (items : list A) index item,
    length (replace_nth items index item) = length items.
Proof.
  induction items as [| head rest IH]; intros index item;
    destruct index; simpl; auto.
Qed.

Lemma nth_error_replace_nth_eq :
  forall {A : Type} (items : list A) index item,
    (index < length items)%nat ->
    nth_error (replace_nth items index item) index = Some item.
Proof.
  induction items as [| head rest IH]; intros index item Hlt;
    destruct index; simpl in *; try lia; auto.
  apply IH.
  lia.
Qed.

Lemma nth_error_replace_nth_neq :
  forall {A : Type} (items : list A) index other item,
    index <> other ->
    nth_error (replace_nth items index item) other =
      nth_error items other.
Proof.
  induction items as [| head rest IH]; intros index other item Hneq.
  - destruct index; destruct other; reflexivity.
  - destruct index as [| index']; destruct other as [| other']; simpl in *.
    + contradiction.
    + reflexivity.
    + reflexivity.
    + apply IH. lia.
Qed.

Lemma replace_entity_shape :
  forall rq index entity,
    eevdf_shape rq ->
    eevdf_shape (replace_entity rq index entity).
Proof.
  intros rq index entity [Hcount Hlen].
  unfold eevdf_shape, replace_entity.
  simpl.
  split; auto.
  rewrite replace_nth_length.
  exact Hlen.
Qed.

Lemma replace_entity_lookup_eq :
  forall rq index entity,
    (index < length (er_entities rq))%nat ->
    lookup_entity (replace_entity rq index entity) index = Some entity.
Proof.
  intros rq index entity Hlt.
  unfold lookup_entity, replace_entity.
  simpl.
  apply nth_error_replace_nth_eq.
  exact Hlt.
Qed.

Lemma replace_entity_lookup_neq :
  forall rq index other entity,
    index <> other ->
    lookup_entity (replace_entity rq index entity) other =
      lookup_entity rq other.
Proof.
  intros rq index other entity Hneq.
  unfold lookup_entity, replace_entity.
  simpl.
  apply nth_error_replace_nth_neq.
  exact Hneq.
Qed.

Lemma replace_entity_active_eq_shaped :
  forall rq index entity,
    eevdf_shape rq ->
    (index < er_entity_count rq)%nat ->
    eevdf_active_entity (replace_entity rq index entity) index entity.
Proof.
  intros rq index entity [Hcount Hlen] Hlt.
  unfold eevdf_active_entity.
  split; simpl; auto.
  unfold replace_entity.
  simpl.
  apply nth_error_replace_nth_eq.
  rewrite Hlen.
  lia.
Qed.

Lemma replace_entity_active_neq :
  forall rq index other old_entity new_entity,
    index <> other ->
    eevdf_active_entity rq other old_entity ->
    eevdf_active_entity
      (replace_entity rq index new_entity)
      other
      old_entity.
Proof.
  intros rq index other old_entity new_entity Hneq [Hlt Hlookup].
  unfold eevdf_active_entity.
  split; simpl; auto.
  unfold replace_entity.
  simpl.
  rewrite nth_error_replace_nth_neq; auto.
Qed.

Lemma replace_entity_active_cases_shaped :
  forall rq index new_entity other entity,
    eevdf_shape rq ->
    eevdf_active_entity
      (replace_entity rq index new_entity)
      other
      entity ->
    (other = index /\ entity = new_entity) \/
    (other <> index /\ eevdf_active_entity rq other entity).
Proof.
  intros rq index new_entity other entity [Hcount Hlen] [Hlt Hlookup].
  simpl in Hlt.
  destruct (Nat.eq_dec other index) as [Heq | Hneq].
  - subst other.
    left.
    split; auto.
    unfold replace_entity in Hlookup.
    simpl in Hlookup.
    rewrite nth_error_replace_nth_eq in Hlookup.
    + inversion Hlookup; reflexivity.
    + rewrite Hlen. lia.
  - right.
    split; auto.
    unfold eevdf_active_entity.
    split; simpl in *; auto.
    unfold replace_entity in Hlookup.
    simpl in Hlookup.
    rewrite nth_error_replace_nth_neq in Hlookup; auto.
Qed.

Lemma replace_entity_inactive_entities_empty_active :
  forall rq index entity,
    eevdf_inactive_entities_empty rq ->
    (index < er_entity_count rq)%nat ->
    eevdf_inactive_entities_empty (replace_entity rq index entity).
Proof.
  intros rq index entity Hinactive Hindex.
  unfold eevdf_inactive_entities_empty in *.
  intros other other_entity Hlookup Hinactive_index.
  simpl in Hinactive_index.
  destruct (Nat.eq_dec other index) as [Heq | Hneq].
  - subst other. lia.
  - unfold replace_entity in Hlookup.
    simpl in Hlookup.
    rewrite nth_error_replace_nth_neq in Hlookup; auto.
    apply Hinactive with (index := other); auto.
Qed.

Lemma append_entity_shape :
  forall rq entity,
    eevdf_shape rq ->
    (er_entity_count rq < eevdf_max_entities)%nat ->
    eevdf_shape (append_entity rq entity).
Proof.
  intros rq entity [Hcount Hlen] Hspace.
  unfold eevdf_shape, append_entity.
  simpl.
  split; [lia |].
  rewrite replace_nth_length.
  exact Hlen.
Qed.

Lemma append_entity_active_cases_shaped :
  forall rq new_entity other entity,
    eevdf_shape rq ->
    (er_entity_count rq < eevdf_max_entities)%nat ->
    eevdf_active_entity (append_entity rq new_entity) other entity ->
    (other = er_entity_count rq /\ entity = new_entity) \/
    ((other < er_entity_count rq)%nat /\
      eevdf_active_entity rq other entity).
Proof.
  intros rq new_entity other entity [Hcount Hlen] Hspace [Hlt Hlookup].
  simpl in Hlt.
  destruct (Nat.eq_dec other (er_entity_count rq)) as [Heq | Hneq].
  - subst other.
    left.
    split; auto.
    unfold append_entity in Hlookup.
    simpl in Hlookup.
    rewrite nth_error_replace_nth_eq in Hlookup.
    + inversion Hlookup; reflexivity.
    + rewrite Hlen. exact Hspace.
  - right.
    split; [lia |].
    unfold eevdf_active_entity.
    split; [lia |].
    unfold append_entity in Hlookup.
    simpl in Hlookup.
    rewrite nth_error_replace_nth_neq in Hlookup; auto.
Qed.

Lemma append_entity_inactive_entities_empty :
  forall rq entity,
    eevdf_inactive_entities_empty rq ->
    eevdf_inactive_entities_empty (append_entity rq entity).
Proof.
  intros rq entity Hinactive.
  unfold eevdf_inactive_entities_empty in *.
  intros other other_entity Hlookup Hinactive_index.
  unfold append_entity in Hlookup.
  simpl in *.
  assert (Hneq : er_entity_count rq <> other) by lia.
  rewrite nth_error_replace_nth_neq in Hlookup; auto.
  apply Hinactive with (index := other); auto.
  lia.
Qed.

Lemma find_entity_index_from_lt :
  forall entities remaining thread_id base index,
    find_entity_index_from entities remaining thread_id base = Some index ->
    (index < base + remaining)%nat.
Proof.
  induction entities as [| entity rest IH];
    intros remaining thread_id base index Hfind;
    destruct remaining as [| remaining']; simpl in Hfind; try discriminate.
  destruct
    (andb
      (ee_thread_id entity =? thread_id)
      (is_active_state (ee_state entity))) eqn:Hmatch.
  - inversion Hfind; subst index.
    lia.
  - specialize (IH remaining' thread_id (S base) index Hfind).
    lia.
Qed.

Lemma find_entity_index_lt :
  forall rq thread_id index,
    find_entity_index rq thread_id = Some index ->
    (index < er_entity_count rq)%nat.
Proof.
  intros rq thread_id index Hfind.
  unfold find_entity_index in Hfind.
  destruct (thread_id =? no_thread_id); try discriminate.
  pose proof (find_entity_index_from_lt
    (er_entities rq)
    (er_entity_count rq)
    thread_id
    0%nat
    index
    Hfind) as Hlt.
  simpl in Hlt.
  exact Hlt.
Qed.

Lemma find_entity_index_active :
  forall rq thread_id index entity,
    find_entity_index rq thread_id = Some index ->
    lookup_entity rq index = Some entity ->
    eevdf_active_entity rq index entity.
Proof.
  intros rq thread_id index entity Hfind Hlookup.
  split; auto.
  apply find_entity_index_lt with (thread_id := thread_id).
  exact Hfind.
Qed.

Lemma find_entity_index_from_none_no_match :
  forall entities remaining thread_id base offset entity,
    find_entity_index_from entities remaining thread_id base = None ->
    nth_error entities offset = Some entity ->
    (offset < remaining)%nat ->
    andb
      (ee_thread_id entity =? thread_id)
      (is_active_state (ee_state entity)) = false.
Proof.
  induction entities as [| head rest IH];
    intros remaining thread_id base offset entity Hfind Hlookup Hlt.
  - destruct remaining; simpl in *.
    + lia.
    + destruct offset; simpl in Hlookup; discriminate.
  - destruct remaining as [| remaining']; simpl in *; try lia.
    destruct offset as [| offset'].
    + inversion Hlookup; subst entity.
      destruct (andb
        (ee_thread_id head =? thread_id)
        (is_active_state (ee_state head))) eqn:Hmatch; try discriminate.
      reflexivity.
    + destruct (andb
        (ee_thread_id head =? thread_id)
        (is_active_state (ee_state head))) eqn:Hmatch; try discriminate.
      eapply IH; eauto.
      lia.
Qed.

Lemma find_entity_index_none_no_active_thread :
  forall rq thread_id index entity,
    thread_id <> no_thread_id ->
    find_entity_index rq thread_id = None ->
    eevdf_active_entity rq index entity ->
    is_active_state (ee_state entity) = true ->
    ee_thread_id entity <> thread_id.
Proof.
  intros rq thread_id index entity Hthread Hfind [Hlt Hlookup] Hactive_state.
  unfold find_entity_index in Hfind.
  destruct (thread_id =? no_thread_id) eqn:Hthread_eq.
  - apply Z.eqb_eq in Hthread_eq. contradiction.
  - pose proof (find_entity_index_from_none_no_match
      (er_entities rq)
      (er_entity_count rq)
      thread_id
      0%nat
      index
      entity
      Hfind
      Hlookup
      Hlt) as Hmatch.
    intro Hsame.
    rewrite Hsame in Hmatch.
    rewrite Z.eqb_refl in Hmatch.
    rewrite Hactive_state in Hmatch.
    discriminate.
Qed.

Lemma z_max_left_when_ge :
  forall lhs rhs,
    rhs <= lhs ->
    z_max lhs rhs = lhs.
Proof.
  intros lhs rhs Hge.
  unfold z_max.
  destruct (lhs <? rhs) eqn:Hlt.
  - apply Z.ltb_lt in Hlt. lia.
  - reflexivity.
Qed.

Lemma invariant_live_deadline_at_vruntime :
  forall rq index entity,
    eevdf_invariant rq ->
    eevdf_active_entity rq index entity ->
    live_for_min entity = true ->
    exists slice,
      weighted_slice (ee_slice_ns entity) (ee_weight entity) = Some slice /\
      ee_eligible_time entity = ee_vruntime entity /\
      ee_deadline entity = ee_vruntime entity + slice.
Proof.
  intros rq index entity Hinv Hactive Hlive_entity.
  destruct Hinv as
    [_Hshape
    [_Hinactive
    [_Hunique
    [_Hpositive
    [Hlive
    [[Hmin_lower _Hmin_attained]
    [_Hvirtual _Hrunnable_count]]]]]]].
  pose proof (Hlive index entity Hactive Hlive_entity) as
    [slice [Hslice [Heligible [_Hfloor Hdeadline]]]].
  pose proof (Hmin_lower index entity Hactive Hlive_entity) as Hmin.
  exists slice.
  repeat split; auto.
  - rewrite Heligible.
    apply z_max_left_when_ge.
    exact Hmin.
  - rewrite Heligible in Hdeadline.
    rewrite z_max_left_when_ge in Hdeadline; auto.
Qed.

Lemma refresh_deadline_at_vruntime_if_floor_lower :
  forall entity floor refreshed,
    floor <= ee_vruntime entity ->
    refresh_deadline entity floor = Some refreshed ->
    floor <= ee_vruntime refreshed /\
    exists slice,
      weighted_slice (ee_slice_ns refreshed) (ee_weight refreshed) =
        Some slice /\
      ee_eligible_time refreshed = ee_vruntime refreshed /\
      ee_deadline refreshed = ee_vruntime refreshed + slice.
Proof.
  intros entity floor refreshed Hfloor Hrefresh.
  pose proof (refresh_deadline_success entity floor refreshed Hrefresh) as
    Hsuccess.
  destruct Hsuccess as
    [_Hthread
    [_Hgeneration
    [Hweight
    [Hslice_ns
    [_Hservice
    [Hvruntime
    [_Hstate Hdeadline_info]]]]]]].
  destruct Hdeadline_info as
    [slice [Hslice [Heligible [_Heligible_floor Hdeadline]]]].
  split.
  - rewrite Hvruntime. exact Hfloor.
  - exists slice.
    repeat split.
    + rewrite Hslice_ns.
      rewrite Hweight.
      exact Hslice.
    + rewrite Hvruntime.
      rewrite Heligible.
      apply z_max_left_when_ge.
      exact Hfloor.
    + rewrite Hvruntime.
      rewrite Hdeadline.
      rewrite Heligible.
      rewrite z_max_left_when_ge; auto.
Qed.

Lemma min_vruntime_from_found_le_current :
  forall entities current,
    min_vruntime_from current true entities <= current.
Proof.
  induction entities as [| entity rest IH]; intros current; simpl.
  - lia.
  - destruct (live_for_min entity) eqn:Hlive.
    + destruct (ee_vruntime entity <? current) eqn:Hlt.
      * apply Z.ltb_lt in Hlt.
        specialize (IH (ee_vruntime entity)).
        lia.
      * exact (IH current).
    + exact (IH current).
Qed.

Lemma min_vruntime_from_lower_bound :
  forall entities current found index entity,
    nth_error entities index = Some entity ->
    live_for_min entity = true ->
    min_vruntime_from current found entities <= ee_vruntime entity.
Proof.
  induction entities as [| head rest IH];
    intros current found index entity Hlookup Hlive; destruct index; simpl in *;
    try discriminate.
  - inversion Hlookup; subst entity.
    rewrite Hlive.
    destruct found.
    + destruct (ee_vruntime head <? current) eqn:Hlt.
      * apply min_vruntime_from_found_le_current.
      * apply Z.ltb_ge in Hlt.
        pose proof (min_vruntime_from_found_le_current rest current).
        lia.
    + apply min_vruntime_from_found_le_current.
  - destruct (live_for_min head) eqn:Hhead_live.
    + destruct found.
      * destruct (ee_vruntime head <? current) eqn:_Hlt;
          eapply IH; eauto.
      * eapply IH; eauto.
    + eapply IH; eauto.
Qed.

Lemma min_vruntime_from_found_attained_or_current :
  forall entities current,
    min_vruntime_from current true entities = current \/
    exists index entity,
      nth_error entities index = Some entity /\
      live_for_min entity = true /\
      ee_vruntime entity = min_vruntime_from current true entities.
Proof.
  induction entities as [| head rest IH]; intros current; simpl.
  - left. reflexivity.
  - destruct (live_for_min head) eqn:Hhead_live.
    + destruct (ee_vruntime head <? current) eqn:Hlt.
      * destruct (IH (ee_vruntime head)) as [Hsame | Hfound].
        -- right.
           exists 0%nat.
           exists head.
           repeat split; auto.
        -- destruct Hfound as [index [entity [Hlookup [Hlive Hvruntime]]]].
           right.
           exists (S index).
           exists entity.
           repeat split; auto.
      * destruct (IH current) as [Hsame | Hfound].
        -- left. exact Hsame.
        -- destruct Hfound as [index [entity [Hlookup [Hlive Hvruntime]]]].
           right.
           exists (S index).
           exists entity.
           repeat split; auto.
    + destruct (IH current) as [Hsame | Hfound].
      * left. exact Hsame.
      * destruct Hfound as [index [entity [Hlookup [Hlive Hvruntime]]]].
        right.
        exists (S index).
        exists entity.
        repeat split; auto.
Qed.

Lemma min_vruntime_from_unfound_attained :
  forall entities current index entity,
    nth_error entities index = Some entity ->
    live_for_min entity = true ->
    exists min_index min_entity,
      nth_error entities min_index = Some min_entity /\
      live_for_min min_entity = true /\
      ee_vruntime min_entity = min_vruntime_from current false entities.
Proof.
  induction entities as [| head rest IH];
    intros current index entity Hlookup Hlive; destruct index; simpl in *;
    try discriminate.
  - inversion Hlookup; subst entity.
    rewrite Hlive.
    destruct (min_vruntime_from_found_attained_or_current
      rest
      (ee_vruntime head)) as [Hsame | Hfound].
    + exists 0%nat.
      exists head.
      repeat split; auto.
    + destruct Hfound as [min_index [min_entity [Hmin_lookup [Hmin_live Hmin_vruntime]]]].
      exists (S min_index).
      exists min_entity.
      repeat split; auto.
  - destruct (live_for_min head) eqn:Hhead_live.
    + destruct (min_vruntime_from_found_attained_or_current
        rest
        (ee_vruntime head)) as [Hsame | Hfound].
      * exists 0%nat.
        exists head.
        repeat split; auto.
      * destruct Hfound as [min_index [min_entity [Hmin_lookup [Hmin_live Hmin_vruntime]]]].
        exists (S min_index).
        exists min_entity.
        repeat split; auto.
    + destruct (IH current index entity Hlookup Hlive) as
        [min_index [min_entity [Hmin_lookup [Hmin_live Hmin_vruntime]]]].
      exists (S min_index).
      exists min_entity.
      repeat split; auto.
Qed.

Lemma refresh_runqueue_shape :
  forall rq,
    eevdf_shape rq ->
    eevdf_shape (refresh_runqueue rq).
Proof.
  intros rq Hshape.
  unfold eevdf_shape, refresh_runqueue in *.
  simpl.
  exact Hshape.
Qed.

Lemma refresh_runqueue_inactive_entities_empty :
  forall rq,
    eevdf_inactive_entities_empty rq ->
    eevdf_inactive_entities_empty (refresh_runqueue rq).
Proof.
  intros rq Hinactive.
  unfold eevdf_inactive_entities_empty, refresh_runqueue in *.
  simpl.
  exact Hinactive.
Qed.

Lemma refresh_runqueue_runnable_count_consistent :
  forall rq,
    eevdf_runnable_count_consistent (refresh_runqueue rq).
Proof.
  intros rq.
  unfold eevdf_runnable_count_consistent, refresh_runqueue.
  simpl.
  reflexivity.
Qed.

Lemma refresh_runqueue_virtual_time_consistent :
  forall rq,
    eevdf_virtual_time_consistent (refresh_runqueue rq).
Proof.
  intros rq.
  unfold eevdf_virtual_time_consistent, refresh_runqueue.
  simpl.
  apply z_max_ge_r.
Qed.

Lemma refresh_runqueue_min_vruntime_lower_bound :
  forall rq,
    eevdf_min_vruntime_lower_bound (refresh_runqueue rq).
Proof.
  intros rq index entity Hactive Hlive.
  unfold eevdf_active_entity in Hactive.
  destruct Hactive as [Hlt Hlookup].
  unfold eevdf_min_vruntime_lower_bound, refresh_runqueue in *.
  simpl in *.
  unfold computed_min_vruntime.
  eapply min_vruntime_from_lower_bound with (index := index).
  - rewrite nth_error_firstn.
    apply Nat.ltb_lt in Hlt.
    rewrite Hlt.
    exact Hlookup.
  - exact Hlive.
Qed.

Lemma refresh_runqueue_min_vruntime_attained :
  forall rq,
    eevdf_min_vruntime_attained (refresh_runqueue rq).
Proof.
  intros rq index entity Hactive Hlive.
  unfold eevdf_active_entity in Hactive.
  destruct Hactive as [Hlt Hlookup].
  unfold refresh_runqueue in *.
  simpl in *.
  assert (Hactive_lookup :
    nth_error
      (firstn (er_entity_count rq) (er_entities rq))
      index = Some entity).
  {
    rewrite nth_error_firstn.
    apply Nat.ltb_lt in Hlt.
    rewrite Hlt.
    exact Hlookup.
  }
  destruct (min_vruntime_from_unfound_attained
    (firstn (er_entity_count rq) (er_entities rq))
    (er_min_vruntime rq)
    index
    entity
    Hactive_lookup
    Hlive) as
    [min_index [min_entity [Hmin_lookup [Hmin_live Hmin_vruntime]]]].
  exists min_index.
  exists min_entity.
  split.
  - unfold eevdf_active_entity.
    rewrite nth_error_firstn in Hmin_lookup.
    destruct (min_index <? er_entity_count rq)%nat eqn:Hmin_lt;
      try discriminate.
    apply Nat.ltb_lt in Hmin_lt.
    split; auto.
  - split; auto.
Qed.

Lemma refresh_runqueue_min_vruntime_consistent :
  forall rq,
    eevdf_min_vruntime_consistent (refresh_runqueue rq).
Proof.
  intros rq.
  split.
  - apply refresh_runqueue_min_vruntime_lower_bound.
  - apply refresh_runqueue_min_vruntime_attained.
Qed.

Lemma refresh_active_entity_iff :
  forall rq index entity,
    eevdf_active_entity (refresh_runqueue rq) index entity <->
    eevdf_active_entity rq index entity.
Proof.
  intros rq index entity.
  unfold eevdf_active_entity, refresh_runqueue.
  simpl.
  tauto.
Qed.

Lemma refresh_runqueue_active_thread_ids_unique :
  forall rq,
    eevdf_active_thread_ids_unique rq ->
    eevdf_active_thread_ids_unique (refresh_runqueue rq).
Proof.
  intros rq Hunique.
  unfold eevdf_active_thread_ids_unique.
  intros i j lhs rhs Hlhs Hrhs Hlhs_nonzero Hthreads_eq
    Hlhs_active Hrhs_active.
  apply refresh_active_entity_iff in Hlhs.
  apply refresh_active_entity_iff in Hrhs.
  eapply Hunique; eauto.
Qed.

Lemma refresh_runqueue_positive_entity_params :
  forall rq,
    eevdf_positive_entity_params rq ->
    eevdf_positive_entity_params (refresh_runqueue rq).
Proof.
  intros rq Hpositive.
  unfold eevdf_positive_entity_params.
  intros index entity Hactive Hactive_state.
  apply refresh_active_entity_iff in Hactive.
  eapply Hpositive; eauto.
Qed.

Lemma replace_entity_preserves_active_thread_ids_unique_same_thread :
  forall rq index old_entity new_entity,
    eevdf_shape rq ->
    eevdf_active_thread_ids_unique rq ->
    eevdf_active_entity rq index old_entity ->
    is_active_state (ee_state old_entity) = true ->
    ee_thread_id new_entity = ee_thread_id old_entity ->
    eevdf_active_thread_ids_unique
      (replace_entity rq index new_entity).
Proof.
  intros rq index old_entity new_entity Hshape Hunique Hold_active
    Hold_active_state Hthread.
  unfold eevdf_active_thread_ids_unique.
  intros i j lhs rhs Hlhs_new Hrhs_new Hlhs_nonzero Hthreads_eq
    Hlhs_active_state Hrhs_active_state.
  pose proof (replace_entity_active_cases_shaped
    rq index new_entity i lhs Hshape Hlhs_new) as Hlhs_cases.
  pose proof (replace_entity_active_cases_shaped
    rq index new_entity j rhs Hshape Hrhs_new) as Hrhs_cases.
  destruct Hlhs_cases as [[Hi Hlhs] | [Hi_ne Hlhs_old]];
    destruct Hrhs_cases as [[Hj Hrhs] | [Hj_ne Hrhs_old]].
  - subst i j lhs rhs. reflexivity.
  - subst i lhs.
    eapply Hunique.
    + exact Hold_active.
    + exact Hrhs_old.
    + rewrite <- Hthread. exact Hlhs_nonzero.
    + rewrite <- Hthread. exact Hthreads_eq.
    + exact Hold_active_state.
    + exact Hrhs_active_state.
  - subst j rhs.
    eapply Hunique.
    + exact Hlhs_old.
    + exact Hold_active.
    + exact Hlhs_nonzero.
    + rewrite Hthread in Hthreads_eq. exact Hthreads_eq.
    + exact Hlhs_active_state.
    + exact Hold_active_state.
  - eapply Hunique; eauto.
Qed.

Lemma replace_entity_preserves_positive_entity_params_same_params :
  forall rq index old_entity new_entity,
    eevdf_shape rq ->
    eevdf_positive_entity_params rq ->
    eevdf_active_entity rq index old_entity ->
    is_active_state (ee_state old_entity) = true ->
    ee_weight new_entity = ee_weight old_entity ->
    ee_slice_ns new_entity = ee_slice_ns old_entity ->
    eevdf_positive_entity_params
      (replace_entity rq index new_entity).
Proof.
  intros rq index old_entity new_entity Hshape Hpositive Hold_active
    Hold_active_state Hweight Hslice_ns.
  unfold eevdf_positive_entity_params.
  intros other active_entity Hactive_new Hactive_state.
  pose proof (replace_entity_active_cases_shaped
    rq index new_entity other active_entity Hshape Hactive_new) as Hcases.
  destruct Hcases as [[Hother_eq Hentity_eq] | [Hneq Hactive_old]].
  - subst other active_entity.
    rewrite Hweight.
    rewrite Hslice_ns.
    eapply Hpositive; eauto.
  - eapply Hpositive; eauto.
Qed.

Lemma refresh_replace_preserves_live_deadlines_consistent :
  forall rq index old_entity new_entity,
    eevdf_invariant rq ->
    eevdf_active_entity rq index old_entity ->
    (live_for_min new_entity = true ->
      er_min_vruntime rq <= ee_vruntime new_entity /\
      exists slice,
        weighted_slice (ee_slice_ns new_entity) (ee_weight new_entity) =
          Some slice /\
        ee_eligible_time new_entity = ee_vruntime new_entity /\
        ee_deadline new_entity = ee_vruntime new_entity + slice) ->
    eevdf_live_deadlines_consistent
      (refresh_runqueue (replace_entity rq index new_entity)).
Proof.
  intros rq index old_entity new_entity Hinv Hold_active Hnew_deadline.
  pose proof Hinv as Hinv_full.
  destruct Hinv as
    [Hshape
    [_Hinactive
    [_Hunique
    [_Hpositive
    [_Hlive
    [_Hmin
    [_Hvirtual _Hrunnable_count]]]]]]].
  unfold eevdf_live_deadlines_consistent.
  intros other active_entity Hactive_final Hlive_final.
  pose proof Hactive_final as Hactive_for_min.
  apply refresh_active_entity_iff in Hactive_for_min.
  pose proof (refresh_runqueue_min_vruntime_lower_bound
    (replace_entity rq index new_entity)
    other
    active_entity
    Hactive_final
    Hlive_final) as Hnew_min_lower.
  pose proof (replace_entity_active_cases_shaped
    rq index new_entity other active_entity Hshape Hactive_for_min) as Hcases.
  destruct Hcases as [[Hother_eq Hentity_eq] | [Hneq Hactive_old]].
  - subst other active_entity.
    destruct (Hnew_deadline Hlive_final) as
      [_Hold_min_lower [slice [Hslice [Heligible Hdeadline]]]].
    exists slice.
    split; [exact Hslice |].
    split.
    + rewrite Heligible.
      symmetry.
      apply z_max_left_when_ge.
      exact Hnew_min_lower.
    + split.
      * rewrite Heligible. exact Hnew_min_lower.
      * rewrite Hdeadline.
        rewrite Heligible.
        reflexivity.
  - destruct (invariant_live_deadline_at_vruntime
      rq other active_entity Hinv_full Hactive_old Hlive_final) as
      [slice [Hslice [Heligible Hdeadline]]].
    exists slice.
    split; [exact Hslice |].
    split.
    + rewrite Heligible.
      symmetry.
      apply z_max_left_when_ge.
      exact Hnew_min_lower.
    + split.
      * rewrite Heligible. exact Hnew_min_lower.
      * rewrite Hdeadline.
        rewrite Heligible.
        reflexivity.
Qed.

Lemma refresh_replace_preserves_invariant_same_thread :
  forall rq index old_entity new_entity,
    eevdf_invariant rq ->
    eevdf_active_entity rq index old_entity ->
    is_active_state (ee_state old_entity) = true ->
    ee_thread_id new_entity = ee_thread_id old_entity ->
    ee_weight new_entity = ee_weight old_entity ->
    ee_slice_ns new_entity = ee_slice_ns old_entity ->
    (live_for_min new_entity = true ->
      er_min_vruntime rq <= ee_vruntime new_entity /\
      exists slice,
        weighted_slice (ee_slice_ns new_entity) (ee_weight new_entity) =
          Some slice /\
        ee_eligible_time new_entity = ee_vruntime new_entity /\
        ee_deadline new_entity = ee_vruntime new_entity + slice) ->
    eevdf_invariant (refresh_runqueue (replace_entity rq index new_entity)).
Proof.
  intros rq index old_entity new_entity Hinv Hold_active Hold_active_state
    Hthread Hweight Hslice_ns Hnew_deadline.
  pose proof Hinv as Hinv_full.
  destruct Hinv as
    [Hshape
    [Hinactive
    [Hunique
    [Hpositive
    [_Hlive
    [_Hmin
    [_Hvirtual _Hrunnable_count]]]]]]].
  apply eevdf_invariant_intro.
  - apply refresh_runqueue_shape.
    apply replace_entity_shape.
    exact Hshape.
  - apply refresh_runqueue_inactive_entities_empty.
    apply replace_entity_inactive_entities_empty_active; auto.
    exact (proj1 Hold_active).
  - apply refresh_runqueue_active_thread_ids_unique.
    apply replace_entity_preserves_active_thread_ids_unique_same_thread
      with (old_entity := old_entity); auto.
  - apply refresh_runqueue_positive_entity_params.
    apply replace_entity_preserves_positive_entity_params_same_params
      with (old_entity := old_entity); auto.
  - apply refresh_replace_preserves_live_deadlines_consistent
      with (old_entity := old_entity); auto.
  - apply refresh_runqueue_min_vruntime_consistent.
  - apply refresh_runqueue_virtual_time_consistent.
  - apply refresh_runqueue_runnable_count_consistent.
Qed.

Lemma append_entity_preserves_active_thread_ids_unique :
  forall rq thread_id new_entity,
    eevdf_shape rq ->
    eevdf_active_thread_ids_unique rq ->
    (er_entity_count rq < eevdf_max_entities)%nat ->
    thread_id <> no_thread_id ->
    find_entity_index rq thread_id = None ->
    ee_thread_id new_entity = thread_id ->
    eevdf_active_thread_ids_unique (append_entity rq new_entity).
Proof.
  intros rq thread_id new_entity Hshape Hunique Hspace Hthread Hfind Hnew_thread.
  unfold eevdf_active_thread_ids_unique.
  intros i j lhs rhs Hlhs_new Hrhs_new Hlhs_nonzero Hthreads_eq
    Hlhs_active_state Hrhs_active_state.
  pose proof (append_entity_active_cases_shaped
    rq new_entity i lhs Hshape Hspace Hlhs_new) as Hlhs_cases.
  pose proof (append_entity_active_cases_shaped
    rq new_entity j rhs Hshape Hspace Hrhs_new) as Hrhs_cases.
  destruct Hlhs_cases as [[Hi Hlhs] | [Hi_old Hlhs_old]];
    destruct Hrhs_cases as [[Hj Hrhs] | [Hj_old Hrhs_old]].
  - subst i j lhs rhs. reflexivity.
  - subst i lhs.
    exfalso.
    apply (find_entity_index_none_no_active_thread
      rq thread_id j rhs Hthread Hfind Hrhs_old Hrhs_active_state).
    rewrite <- Hthreads_eq.
    exact Hnew_thread.
  - subst j rhs.
    exfalso.
    apply (find_entity_index_none_no_active_thread
      rq thread_id i lhs Hthread Hfind Hlhs_old Hlhs_active_state).
    rewrite Hthreads_eq.
    exact Hnew_thread.
  - eapply Hunique; eauto.
Qed.

Lemma append_entity_preserves_positive_entity_params :
  forall rq new_entity,
    eevdf_shape rq ->
    eevdf_positive_entity_params rq ->
    (er_entity_count rq < eevdf_max_entities)%nat ->
    0 < ee_weight new_entity ->
    0 < ee_slice_ns new_entity ->
    eevdf_positive_entity_params (append_entity rq new_entity).
Proof.
  intros rq new_entity Hshape Hpositive Hspace Hnew_weight Hnew_slice.
  unfold eevdf_positive_entity_params.
  intros other entity Hactive_new Hactive_state.
  pose proof (append_entity_active_cases_shaped
    rq new_entity other entity Hshape Hspace Hactive_new) as Hcases.
  destruct Hcases as [[Hother Hentity] | [_Hold_lt Hactive_old]].
  - subst other entity.
    split; auto.
  - eapply Hpositive; eauto.
Qed.

Lemma refresh_append_preserves_live_deadlines_consistent :
  forall rq new_entity,
    eevdf_invariant rq ->
    eevdf_shape rq ->
    (er_entity_count rq < eevdf_max_entities)%nat ->
    (live_for_min new_entity = true ->
      er_min_vruntime rq <= ee_vruntime new_entity /\
      exists slice,
        weighted_slice (ee_slice_ns new_entity) (ee_weight new_entity) =
          Some slice /\
        ee_eligible_time new_entity = ee_vruntime new_entity /\
        ee_deadline new_entity = ee_vruntime new_entity + slice) ->
    eevdf_live_deadlines_consistent
      (refresh_runqueue (append_entity rq new_entity)).
Proof.
  intros rq new_entity Hinv Hshape Hspace Hnew_deadline.
  unfold eevdf_live_deadlines_consistent.
  intros other active_entity Hactive_final Hlive_final.
  pose proof Hactive_final as Hactive_for_min.
  apply refresh_active_entity_iff in Hactive_for_min.
  pose proof (refresh_runqueue_min_vruntime_lower_bound
    (append_entity rq new_entity)
    other
    active_entity
    Hactive_final
    Hlive_final) as Hnew_min_lower.
  pose proof (append_entity_active_cases_shaped
    rq new_entity other active_entity Hshape Hspace Hactive_for_min) as Hcases.
  destruct Hcases as [[Hother Hentity] | [_Hold_lt Hactive_old]].
  - subst other active_entity.
    destruct (Hnew_deadline Hlive_final) as
      [_Hold_min_lower [slice [Hslice [Heligible Hdeadline]]]].
    exists slice.
    split; [exact Hslice |].
    split.
    + rewrite Heligible.
      symmetry.
      apply z_max_left_when_ge.
      exact Hnew_min_lower.
    + split.
      * rewrite Heligible. exact Hnew_min_lower.
      * rewrite Hdeadline.
        rewrite Heligible.
        reflexivity.
  - destruct (invariant_live_deadline_at_vruntime
      rq other active_entity Hinv Hactive_old Hlive_final) as
      [slice [Hslice [Heligible Hdeadline]]].
    exists slice.
    split; [exact Hslice |].
    split.
    + rewrite Heligible.
      symmetry.
      apply z_max_left_when_ge.
      exact Hnew_min_lower.
    + split.
      * rewrite Heligible. exact Hnew_min_lower.
      * rewrite Hdeadline.
        rewrite Heligible.
        reflexivity.
Qed.

Lemma refresh_append_preserves_invariant :
  forall rq thread_id new_entity,
    eevdf_invariant rq ->
    (er_entity_count rq < eevdf_max_entities)%nat ->
    thread_id <> no_thread_id ->
    find_entity_index rq thread_id = None ->
    ee_thread_id new_entity = thread_id ->
    0 < ee_weight new_entity ->
    0 < ee_slice_ns new_entity ->
    (live_for_min new_entity = true ->
      er_min_vruntime rq <= ee_vruntime new_entity /\
      exists slice,
        weighted_slice (ee_slice_ns new_entity) (ee_weight new_entity) =
          Some slice /\
        ee_eligible_time new_entity = ee_vruntime new_entity /\
        ee_deadline new_entity = ee_vruntime new_entity + slice) ->
    eevdf_invariant (refresh_runqueue (append_entity rq new_entity)).
Proof.
  intros rq thread_id new_entity Hinv Hspace Hthread Hfind Hnew_thread
    Hnew_weight Hnew_slice Hnew_deadline.
  pose proof Hinv as Hinv_full.
  destruct Hinv as
    [Hshape
    [Hinactive
    [Hunique
    [Hpositive
    [_Hlive
    [_Hmin
    [_Hvirtual _Hrunnable_count]]]]]]].
  apply eevdf_invariant_intro.
  - apply refresh_runqueue_shape.
    apply append_entity_shape; auto.
  - apply refresh_runqueue_inactive_entities_empty.
    apply append_entity_inactive_entities_empty.
    exact Hinactive.
  - apply refresh_runqueue_active_thread_ids_unique.
    apply append_entity_preserves_active_thread_ids_unique
      with (thread_id := thread_id); auto.
  - apply refresh_runqueue_positive_entity_params.
    apply append_entity_preserves_positive_entity_params; auto.
  - apply refresh_append_preserves_live_deadlines_consistent; auto.
  - apply refresh_runqueue_min_vruntime_consistent.
  - apply refresh_runqueue_virtual_time_consistent.
  - apply refresh_runqueue_runnable_count_consistent.
Qed.

Lemma recount_runqueue_shape :
  forall rq,
    eevdf_shape rq ->
    eevdf_shape (recount_runqueue rq).
Proof.
  intros rq Hshape.
  unfold eevdf_shape, recount_runqueue in *.
  simpl.
  exact Hshape.
Qed.

Lemma recount_runqueue_inactive_entities_empty :
  forall rq,
    eevdf_inactive_entities_empty rq ->
    eevdf_inactive_entities_empty (recount_runqueue rq).
Proof.
  intros rq Hinactive.
  unfold eevdf_inactive_entities_empty, recount_runqueue in *.
  simpl.
  exact Hinactive.
Qed.

Lemma recount_runqueue_virtual_time_consistent :
  forall rq,
    eevdf_virtual_time_consistent rq ->
    eevdf_virtual_time_consistent (recount_runqueue rq).
Proof.
  intros rq Hvirtual.
  unfold eevdf_virtual_time_consistent, recount_runqueue in *.
  simpl.
  exact Hvirtual.
Qed.

Lemma recount_runqueue_runnable_count_consistent :
  forall rq,
    eevdf_runnable_count_consistent (recount_runqueue rq).
Proof.
  intros rq.
  unfold eevdf_runnable_count_consistent, recount_runqueue.
  simpl.
  reflexivity.
Qed.

Lemma set_entity_state_live_running :
  forall entity,
    ee_state entity = ERunnable ->
    live_for_min (set_entity_state entity ERunning) = true.
Proof.
  intros entity _.
  reflexivity.
Qed.

Lemma set_entity_state_running_keeps_deadline_fields :
  forall entity,
    ee_weight (set_entity_state entity ERunning) = ee_weight entity /\
    ee_slice_ns (set_entity_state entity ERunning) = ee_slice_ns entity /\
    ee_vruntime (set_entity_state entity ERunning) = ee_vruntime entity /\
    ee_eligible_time (set_entity_state entity ERunning) =
      ee_eligible_time entity /\
    ee_deadline (set_entity_state entity ERunning) = ee_deadline entity.
Proof.
  intros entity.
  repeat split.
Qed.

Lemma set_entity_state_running_active :
  forall entity,
    is_active_state (ee_state entity) = true ->
    is_active_state (ee_state (set_entity_state entity ERunning)) = true.
Proof.
  intros entity _.
  reflexivity.
Qed.

Lemma set_entity_state_running_thread_id :
  forall entity,
    ee_thread_id (set_entity_state entity ERunning) = ee_thread_id entity.
Proof.
  reflexivity.
Qed.

Lemma set_entity_state_running_vruntime :
  forall entity,
    ee_vruntime (set_entity_state entity ERunning) = ee_vruntime entity.
Proof.
  reflexivity.
Qed.

Lemma set_entity_state_running_live :
  forall entity,
    live_for_min (set_entity_state entity ERunning) = true.
Proof.
  intros entity.
  reflexivity.
Qed.

Lemma recount_active_entity_iff :
  forall rq index entity,
    eevdf_active_entity (recount_runqueue rq) index entity <->
    eevdf_active_entity rq index entity.
Proof.
  intros rq index entity.
  unfold eevdf_active_entity, recount_runqueue.
  simpl.
  tauto.
Qed.

Theorem reset_preserves_invariant :
  forall rq,
    eevdf_invariant (eevdf_result_rq (eevdf_reset rq)).
Proof.
  intros rq.
  unfold eevdf_reset, ok, eevdf_empty_runqueue.
  unfold eevdf_invariant.
  split.
  - unfold eevdf_shape.
    split; simpl; [lia |].
    reflexivity.
  - split.
    + unfold eevdf_inactive_entities_empty.
      intros index entity Hlookup _.
      apply repeat_empty_entity_state with (index := index).
      exact Hlookup.
    + split.
      * unfold eevdf_active_thread_ids_unique, eevdf_active_entity.
        intros i j lhs rhs [Hilt _] _ _ _ _ _.
        simpl in Hilt. lia.
      * split.
        -- unfold eevdf_positive_entity_params, eevdf_active_entity.
           intros index entity [Hlt _] _.
           simpl in Hlt. lia.
        -- split.
           ++ unfold eevdf_live_deadlines_consistent, eevdf_active_entity.
              intros index entity [Hlt _] _.
              simpl in Hlt. lia.
           ++ split.
              ** unfold eevdf_min_vruntime_consistent.
                 split.
                 --- unfold eevdf_min_vruntime_lower_bound, eevdf_active_entity.
                     intros index entity [Hlt _] _.
                     simpl in Hlt. lia.
                 --- unfold eevdf_min_vruntime_attained, eevdf_active_entity.
                     intros index entity [Hlt _] _.
                     simpl in Hlt. lia.
              ** split.
                 --- unfold eevdf_virtual_time_consistent.
                     simpl. lia.
                 --- unfold eevdf_runnable_count_consistent.
                     simpl. reflexivity.
Qed.

Theorem pick_preserves_invariant :
  forall rq rq' picked,
    eevdf_invariant rq ->
    eevdf_pick rq = (rq', picked) ->
    eevdf_invariant rq'.
Proof.
  intros rq rq' picked Hinv Hpick.
  unfold eevdf_pick in Hpick.
  destruct Hinv as
    [Hshape
      [Hinactive
      [Hunique
      [Hpositive
      [Hlive
      [Hmin
      [Hvirtual Hrunnable_count]]]]]]].
  destruct (best_eligible rq (er_virtual_time rq)) as [[best_index best_entity] |]
    eqn:Hbest.
  - inversion Hpick; subst rq' picked.
    apply eevdf_invariant_intro; auto.
  - destruct (next_eligible rq) as [next_time |] eqn:Hnext.
    + inversion Hpick; subst rq' picked.
      pose proof (next_eligible_some_spec rq next_time Hnext) as
        [[future_index [future_entity [Hfuture_active [Hfuture_state Hfuture_time]]]]
          _].
      assert (Hnext_ge_min : er_min_vruntime rq <= next_time).
      {
        pose proof (Hlive
          future_index
          future_entity
          Hfuture_active
          (runnable_entity_live future_entity Hfuture_state)) as
          [slice [_ [_ [Hfloor _]]]].
        lia.
      }
      apply eevdf_invariant_intro.
      * unfold eevdf_shape in *.
        simpl. exact Hshape.
      * unfold eevdf_inactive_entities_empty in *.
        simpl. exact Hinactive.
      * unfold eevdf_active_thread_ids_unique in *.
        simpl. exact Hunique.
      * unfold eevdf_positive_entity_params in *.
        simpl. exact Hpositive.
      * unfold eevdf_live_deadlines_consistent in *.
        simpl. exact Hlive.
      * unfold eevdf_min_vruntime_consistent in *.
        simpl. exact Hmin.
      * unfold eevdf_virtual_time_consistent.
        simpl. exact Hnext_ge_min.
      * unfold eevdf_runnable_count_consistent in *.
        simpl. exact Hrunnable_count.
    + inversion Hpick; subst rq' picked.
      apply eevdf_invariant_intro; auto.
Qed.

Theorem add_preserves_invariant :
  forall rq thread_id generation weight slice_ns rq',
    eevdf_invariant rq ->
    eevdf_add rq thread_id generation weight slice_ns = ok rq' ->
    eevdf_invariant rq'.
Proof.
  intros rq thread_id generation weight slice_ns rq' Hinv Hadd.
  unfold eevdf_add in Hadd.
  destruct (thread_id =? no_thread_id) eqn:Hthread_eq; try discriminate.
  apply Z.eqb_neq in Hthread_eq.
  destruct (negb (eevdf_valid_entity_params weight slice_ns))
    eqn:Hparams_neg; try discriminate.
  apply negb_false_iff in Hparams_neg.
  destruct (find_entity_index rq thread_id) as [existing_index |] eqn:Hfind;
    try discriminate.
  destruct (er_entity_count rq <? eevdf_max_entities)%nat eqn:Hspace_bool;
    try discriminate.
  apply Nat.ltb_lt in Hspace_bool.
  destruct (eevdf_make_entity rq thread_id generation weight slice_ns)
    as [entity |] eqn:Hmake; try discriminate.
  inversion Hadd; subst rq'.
  unfold eevdf_make_entity in Hmake.
  set (seed :=
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
    |}) in Hmake.
  pose proof (refresh_deadline_success seed (er_min_vruntime rq) entity Hmake)
    as Hmake_success.
  destruct Hmake_success as
    [Hentity_thread
    [_Hentity_generation
    [Hentity_weight
    [Hentity_slice
    [_Hentity_service
    [_Hentity_vruntime
    [_Hentity_state _Hentity_deadline]]]]]]].
  apply refresh_append_preserves_invariant with (thread_id := thread_id).
  - exact Hinv.
  - exact Hspace_bool.
  - exact Hthread_eq.
  - exact Hfind.
  - subst seed. simpl in Hentity_thread. exact Hentity_thread.
  - subst seed.
    simpl in Hentity_weight.
    rewrite Hentity_weight.
    unfold eevdf_valid_entity_params in Hparams_neg.
    apply andb_true_iff in Hparams_neg as [Hweight_positive _].
    unfold valid_positive in Hweight_positive.
    apply Z.ltb_lt in Hweight_positive.
    exact Hweight_positive.
  - subst seed.
    simpl in Hentity_slice.
    rewrite Hentity_slice.
    unfold eevdf_valid_entity_params in Hparams_neg.
    apply andb_true_iff in Hparams_neg as [_ Hslice_positive].
    unfold valid_positive in Hslice_positive.
    apply Z.ltb_lt in Hslice_positive.
    exact Hslice_positive.
  - intros _Hlive_new.
    apply refresh_deadline_at_vruntime_if_floor_lower
      with (entity := seed) (floor := er_min_vruntime rq); auto.
    subst seed.
    simpl.
    lia.
Qed.

Theorem mark_running_preserves_frame_invariants :
  forall rq thread_id rq',
    eevdf_invariant rq ->
    eevdf_mark_running rq thread_id = ok rq' ->
    eevdf_shape rq' /\
    eevdf_inactive_entities_empty rq' /\
    eevdf_virtual_time_consistent rq' /\
    eevdf_runnable_count_consistent rq'.
Proof.
  intros rq thread_id rq' Hinv Hmark.
  destruct Hinv as
    [Hshape
      [Hinactive
      [_Hunique
      [_Hpositive
      [_Hlive
      [_Hmin
      [Hvirtual _Hrunnable_count]]]]]]].
  destruct (eevdf_mark_running_success_formula rq thread_id rq' Hmark)
    as [index [entity [Hfind [Hlookup [Hstate Hrq']]]]].
  pose proof (find_entity_index_lt rq thread_id index Hfind) as Hindex.
  subst rq'.
  split.
  - apply recount_runqueue_shape.
    apply replace_entity_shape.
    exact Hshape.
  - split.
    + apply recount_runqueue_inactive_entities_empty.
      apply replace_entity_inactive_entities_empty_active; auto.
    + split.
      * apply recount_runqueue_virtual_time_consistent.
        unfold eevdf_virtual_time_consistent in *.
        unfold replace_entity.
        simpl.
        exact Hvirtual.
      * apply recount_runqueue_runnable_count_consistent.
Qed.

Theorem mark_running_preserves_positive_entity_params :
  forall rq thread_id rq',
    eevdf_invariant rq ->
    eevdf_mark_running rq thread_id = ok rq' ->
    eevdf_positive_entity_params rq'.
Proof.
  intros rq thread_id rq' Hinv Hmark.
  destruct Hinv as
    [Hshape
      [_Hinactive
      [_Hunique
      [Hpositive
      [_Hlive
      [_Hmin
      [_Hvirtual _Hrunnable_count]]]]]]].
  destruct (eevdf_mark_running_success_formula rq thread_id rq' Hmark)
    as [index [entity [Hfind [Hlookup [Hstate Hrq']]]]].
  pose proof (find_entity_index_active rq thread_id index entity Hfind Hlookup)
    as Hactive_entity.
  subst rq'.
  unfold eevdf_positive_entity_params.
  intros other active_entity Hactive_new Hactive_state.
  apply (proj1 (recount_active_entity_iff
    (replace_entity rq index (set_entity_state entity ERunning))
    other
    active_entity)) in Hactive_new.
  pose proof (replace_entity_active_cases_shaped
    rq
    index
    (set_entity_state entity ERunning)
    other
    active_entity
    Hshape
    Hactive_new) as Hcases.
  destruct Hcases as [[Hother_eq Hentity_eq] | [Hneq Hactive_old]].
  - subst other active_entity.
    simpl.
    apply Hpositive with (index := index); auto.
    rewrite Hstate.
    reflexivity.
  - apply Hpositive with (index := other); auto.
Qed.

Theorem mark_running_preserves_live_deadlines_consistent :
  forall rq thread_id rq',
    eevdf_invariant rq ->
    eevdf_mark_running rq thread_id = ok rq' ->
    eevdf_live_deadlines_consistent rq'.
Proof.
  intros rq thread_id rq' Hinv Hmark.
  destruct Hinv as
    [Hshape
      [_Hinactive
      [_Hunique
      [_Hpositive
      [Hlive
      [_Hmin
      [_Hvirtual _Hrunnable_count]]]]]]].
  destruct (eevdf_mark_running_success_formula rq thread_id rq' Hmark)
    as [index [entity [Hfind [Hlookup [Hstate Hrq']]]]].
  pose proof (find_entity_index_active rq thread_id index entity Hfind Hlookup)
    as Hactive_entity.
  subst rq'.
  unfold eevdf_live_deadlines_consistent.
  intros other active_entity Hactive_new Hlive_new.
  apply recount_active_entity_iff in Hactive_new.
  pose proof (replace_entity_active_cases_shaped
    rq
    index
    (set_entity_state entity ERunning)
    other
    active_entity
    Hshape
    Hactive_new) as Hcases.
  destruct Hcases as [[Hother_eq Hentity_eq] | [Hneq Hactive_old]].
  - subst other active_entity.
    pose proof (Hlive
      index
      entity
      Hactive_entity
      (runnable_entity_live entity Hstate)) as
      [slice [Hslice [Heligible [Hfloor Hdeadline]]]].
    exists slice.
    simpl.
    repeat split; auto.
  - apply Hlive with (index := other); auto.
Qed.

Theorem mark_running_preserves_min_vruntime_lower_bound :
  forall rq thread_id rq',
    eevdf_invariant rq ->
    eevdf_mark_running rq thread_id = ok rq' ->
    eevdf_min_vruntime_lower_bound rq'.
Proof.
  intros rq thread_id rq' Hinv Hmark.
  destruct Hinv as
    [Hshape
      [_Hinactive
      [_Hunique
      [_Hpositive
      [_Hlive
      [[Hmin_lower _Hmin_attained]
      [_Hvirtual _Hrunnable_count]]]]]]].
  destruct (eevdf_mark_running_success_formula rq thread_id rq' Hmark)
    as [index [entity [Hfind [Hlookup [Hstate Hrq']]]]].
  pose proof (find_entity_index_active rq thread_id index entity Hfind Hlookup)
    as Hactive_entity.
  subst rq'.
  unfold eevdf_min_vruntime_lower_bound.
  intros other active_entity Hactive_new Hlive_new.
  apply recount_active_entity_iff in Hactive_new.
  pose proof (replace_entity_active_cases_shaped
    rq
    index
    (set_entity_state entity ERunning)
    other
    active_entity
    Hshape
    Hactive_new) as Hcases.
  destruct Hcases as [[Hother_eq Hentity_eq] | [Hneq Hactive_old]].
  - subst other active_entity.
    simpl.
    apply Hmin_lower with (index := index); auto.
    apply runnable_entity_live.
    exact Hstate.
  - apply Hmin_lower with (index := other); auto.
Qed.

Theorem mark_running_preserves_min_vruntime_attained :
  forall rq thread_id rq',
    eevdf_invariant rq ->
    eevdf_mark_running rq thread_id = ok rq' ->
    eevdf_min_vruntime_attained rq'.
Proof.
  intros rq thread_id rq' Hinv Hmark.
  destruct Hinv as
    [Hshape
    [_Hinactive
    [_Hunique
    [_Hpositive
    [_Hlive
    [[_Hmin_lower Hmin_attained]
    [_Hvirtual _Hrunnable_count]]]]]]].
  destruct (eevdf_mark_running_success_formula rq thread_id rq' Hmark)
    as [index [entity [Hfind [Hlookup [Hstate Hrq']]]]].
  pose proof (find_entity_index_active rq thread_id index entity Hfind Hlookup)
    as Hactive_entity.
  subst rq'.
  unfold eevdf_min_vruntime_attained.
  intros other active_entity Hactive_new Hlive_new.
  apply recount_active_entity_iff in Hactive_new.
  pose proof (replace_entity_active_cases_shaped
    rq
    index
    (set_entity_state entity ERunning)
    other
    active_entity
    Hshape
    Hactive_new) as Hquery_cases.
  assert (Hquery_exists :
    exists query_index query_entity,
      eevdf_active_entity rq query_index query_entity /\
      live_for_min query_entity = true).
  {
    destruct Hquery_cases as [[Hother_eq Hentity_eq] | [Hneq Hactive_old]].
    - subst other active_entity.
      exists index.
      exists entity.
      split; auto.
      apply runnable_entity_live.
      exact Hstate.
    - exists other.
      exists active_entity.
      split; auto.
  }
  destruct Hquery_exists as
    [query_index [query_entity [Hquery_active Hquery_live]]].
  destruct (Hmin_attained query_index query_entity Hquery_active Hquery_live)
    as [min_index [min_entity [Hmin_active [Hmin_live Hmin_vruntime]]]].
  destruct (Nat.eq_dec min_index index) as [Hmin_eq | Hmin_neq].
  - subst min_index.
    destruct Hmin_active as [Hmin_lt Hmin_lookup].
    unfold lookup_entity in Hlookup.
    rewrite Hlookup in Hmin_lookup.
    inversion Hmin_lookup; subst min_entity.
    exists index.
    exists (set_entity_state entity ERunning).
    split.
    + apply (proj2 (recount_active_entity_iff
        (replace_entity rq index (set_entity_state entity ERunning))
        index
        (set_entity_state entity ERunning))).
      apply replace_entity_active_eq_shaped; auto.
    + split.
      * reflexivity.
      * simpl. exact Hmin_vruntime.
  - exists min_index.
    exists min_entity.
    split.
    + unfold eevdf_active_entity, recount_runqueue, replace_entity.
      destruct Hmin_active as [Hmin_lt Hmin_lookup].
      simpl.
      split; auto.
      rewrite nth_error_replace_nth_neq; auto.
    + split; auto.
Qed.

Theorem mark_running_preserves_active_thread_ids_unique :
  forall rq thread_id rq',
    eevdf_invariant rq ->
    eevdf_mark_running rq thread_id = ok rq' ->
    eevdf_active_thread_ids_unique rq'.
Proof.
  intros rq thread_id rq' Hinv Hmark.
  destruct Hinv as
    [Hshape
    [_Hinactive
    [Hunique
    [_Hpositive
    [_Hlive
    [_Hmin
    [_Hvirtual _Hrunnable_count]]]]]]].
  destruct (eevdf_mark_running_success_formula rq thread_id rq' Hmark)
    as [index [entity [Hfind [Hlookup [Hstate Hrq']]]]].
  pose proof (find_entity_index_active rq thread_id index entity Hfind Hlookup)
    as Hactive_entity.
  subst rq'.
  unfold eevdf_active_thread_ids_unique.
  intros i j lhs rhs Hlhs_new Hrhs_new Hlhs_nonzero Hthreads_eq
    Hlhs_active_state Hrhs_active_state.
  apply (proj1 (recount_active_entity_iff _ _ _)) in Hlhs_new.
  apply (proj1 (recount_active_entity_iff _ _ _)) in Hrhs_new.
  pose proof (replace_entity_active_cases_shaped
    rq
    index
    (set_entity_state entity ERunning)
    i
    lhs
    Hshape
    Hlhs_new) as Hlhs_cases.
  pose proof (replace_entity_active_cases_shaped
    rq
    index
    (set_entity_state entity ERunning)
    j
    rhs
    Hshape
    Hrhs_new) as Hrhs_cases.
  destruct Hlhs_cases as [[Hi Hlhs] | [Hi_ne Hlhs_old]];
    destruct Hrhs_cases as [[Hj Hrhs] | [Hj_ne Hrhs_old]].
  - subst i j lhs rhs. reflexivity.
  - subst i lhs.
    eapply Hunique.
    + exact Hactive_entity.
    + exact Hrhs_old.
    + simpl in Hlhs_nonzero. exact Hlhs_nonzero.
    + simpl in Hthreads_eq. exact Hthreads_eq.
    + rewrite Hstate. reflexivity.
    + exact Hrhs_active_state.
  - subst j rhs.
    eapply Hunique.
    + exact Hlhs_old.
    + exact Hactive_entity.
    + exact Hlhs_nonzero.
    + simpl in Hthreads_eq. exact Hthreads_eq.
    + exact Hlhs_active_state.
    + rewrite Hstate. reflexivity.
  - eapply Hunique; eauto.
Qed.

Theorem mark_running_preserves_min_vruntime_consistent :
  forall rq thread_id rq',
    eevdf_invariant rq ->
    eevdf_mark_running rq thread_id = ok rq' ->
    eevdf_min_vruntime_consistent rq'.
Proof.
  intros rq thread_id rq' Hinv Hmark.
  split.
  - apply mark_running_preserves_min_vruntime_lower_bound
      with (rq := rq) (thread_id := thread_id); auto.
  - apply mark_running_preserves_min_vruntime_attained
      with (rq := rq) (thread_id := thread_id); auto.
Qed.

Theorem mark_running_preserves_invariant :
  forall rq thread_id rq',
    eevdf_invariant rq ->
    eevdf_mark_running rq thread_id = ok rq' ->
    eevdf_invariant rq'.
Proof.
  intros rq thread_id rq' Hinv Hmark.
  destruct (mark_running_preserves_frame_invariants
    rq thread_id rq' Hinv Hmark) as
    [Hshape [Hinactive [Hvirtual Hrunnable_count]]].
  apply eevdf_invariant_intro; auto.
  - apply mark_running_preserves_active_thread_ids_unique
      with (rq := rq) (thread_id := thread_id); auto.
  - apply mark_running_preserves_positive_entity_params
      with (rq := rq) (thread_id := thread_id); auto.
  - apply mark_running_preserves_live_deadlines_consistent
      with (rq := rq) (thread_id := thread_id); auto.
  - apply mark_running_preserves_min_vruntime_consistent
      with (rq := rq) (thread_id := thread_id); auto.
Qed.

Theorem block_preserves_invariant :
  forall rq thread_id rq',
    eevdf_invariant rq ->
    eevdf_block rq thread_id = ok rq' ->
    eevdf_invariant rq'.
Proof.
  intros rq thread_id rq' Hinv Hblock.
  destruct (eevdf_block_success_formula rq thread_id rq' Hblock)
    as [index [entity [Hfind [Hlookup [Hstate Hrq']]]]].
  pose proof (find_entity_index_active rq thread_id index entity Hfind Hlookup)
    as Hactive.
  subst rq'.
  apply refresh_replace_preserves_invariant_same_thread
    with (old_entity := entity).
  - exact Hinv.
  - exact Hactive.
  - destruct entity as
      [old_thread_id old_generation old_weight old_slice_ns old_service_ns
        old_vruntime old_eligible old_deadline old_state];
      destruct old_state; simpl in *; try discriminate; reflexivity.
  - reflexivity.
  - reflexivity.
  - reflexivity.
  - intros Hlive_new. simpl in Hlive_new. discriminate.
Qed.

Theorem exit_preserves_invariant :
  forall rq thread_id rq',
    eevdf_invariant rq ->
    eevdf_exit rq thread_id = ok rq' ->
    eevdf_invariant rq'.
Proof.
  intros rq thread_id rq' Hinv Hexit.
  destruct (eevdf_exit_success_formula rq thread_id rq' Hexit)
    as [index [entity [Hfind [Hlookup [Hnot_empty [Hnot_exited Hrq']]]]]].
  subst rq'.
  (* TODO: replace the old same-slot replacement proof with a compacting
     remove_entity_at preservation lemma. The model now matches the C kernel
     runtime: exit removes the entity from the active range instead of keeping
     an EExited tombstone. *)
  admit.
Admitted.

Theorem wake_preserves_invariant :
  forall rq thread_id rq',
    eevdf_invariant rq ->
    eevdf_wake rq thread_id = ok rq' ->
    eevdf_invariant rq'.
Proof.
  intros rq thread_id rq' Hinv Hwake.
  unfold eevdf_wake in Hwake.
  destruct (find_entity_index rq thread_id) as [index |] eqn:Hfind;
    try discriminate.
  destruct (lookup_entity rq index) as [entity |] eqn:Hlookup;
    try discriminate.
  destruct (ee_state entity) eqn:Hstate; try discriminate.
  destruct (refresh_deadline
    (set_entity_state
      (place_entity_at_floor entity (er_min_vruntime rq))
      ERunnable)
    (er_min_vruntime rq)) as [refreshed |] eqn:Hrefresh;
    try discriminate.
  inversion Hwake; subst rq'.
  pose proof (find_entity_index_active rq thread_id index entity Hfind Hlookup)
    as Hactive.
  pose proof (refresh_deadline_success
    (set_entity_state
      (place_entity_at_floor entity (er_min_vruntime rq))
      ERunnable)
    (er_min_vruntime rq)
    refreshed
    Hrefresh) as
    [Hthread
    [_Hgeneration
    [Hweight
    [Hslice_ns
    [_Hservice
    [_Hvruntime
    [_Hstate_refreshed _Hdeadline]]]]]]].
  apply refresh_replace_preserves_invariant_same_thread
    with (old_entity := entity).
  - exact Hinv.
  - exact Hactive.
  - rewrite Hstate. reflexivity.
  - simpl in Hthread. exact Hthread.
  - simpl in Hweight. exact Hweight.
  - simpl in Hslice_ns. exact Hslice_ns.
  - intros _Hlive_new.
    apply refresh_deadline_at_vruntime_if_floor_lower
      with
        (entity :=
          set_entity_state
            (place_entity_at_floor entity (er_min_vruntime rq))
            ERunnable)
        (floor := er_min_vruntime rq); auto.
    simpl.
    apply z_max_ge_r.
Qed.

Theorem requeue_running_preserves_invariant :
  forall rq thread_id rq',
    eevdf_invariant rq ->
    eevdf_requeue_running rq thread_id = ok rq' ->
    eevdf_invariant rq'.
Proof.
  intros rq thread_id rq' Hinv Hrequeue.
  destruct (eevdf_requeue_running_success_formula rq thread_id rq' Hrequeue)
    as [index [entity [refreshed
      [Hfind [Hlookup [Hstate [Hrefresh Hrq']]]]]]].
  pose proof Hinv as Hinv_full.
  destruct Hinv as
    [_Hshape
    [_Hinactive
    [_Hunique
    [_Hpositive
    [_Hlive
    [[Hmin_lower _Hmin_attained]
    [_Hvirtual _Hrunnable_count]]]]]]].
  pose proof (find_entity_index_active rq thread_id index entity Hfind Hlookup)
    as Hactive.
  pose proof (Hmin_lower index entity Hactive
    (running_entity_live entity Hstate)) as Hfloor.
  pose proof (refresh_deadline_success
    (set_entity_state entity ERunnable)
    (er_min_vruntime rq)
    refreshed
    Hrefresh) as
    [Hthread
    [_Hgeneration
    [Hweight
    [Hslice_ns
    [_Hservice
    [_Hvruntime
    [_Hstate_refreshed _Hdeadline]]]]]]].
  subst rq'.
  apply refresh_replace_preserves_invariant_same_thread
    with (old_entity := entity).
  - exact Hinv_full.
  - exact Hactive.
  - rewrite Hstate. reflexivity.
  - simpl in Hthread. exact Hthread.
  - simpl in Hweight. exact Hweight.
  - simpl in Hslice_ns. exact Hslice_ns.
  - intros _Hlive_new.
    apply refresh_deadline_at_vruntime_if_floor_lower
      with
        (entity := set_entity_state entity ERunnable)
        (floor := er_min_vruntime rq); auto.
Qed.

Theorem charge_preserves_invariant :
  forall rq thread_id runtime_ns rq',
    eevdf_invariant rq ->
    eevdf_charge rq thread_id runtime_ns = ok rq' ->
    eevdf_invariant rq'.
Proof.
  intros rq thread_id runtime_ns rq' Hinv Hcharge.
  destruct (eevdf_charge_success_formula rq thread_id runtime_ns rq' Hcharge)
    as [index [old_entity [charged_entity [refreshed [delta Hsuccess]]]]].
  destruct Hsuccess as
    [Hfind
    [Hlookup
    [Hstate
    [Hdelta
    [Hrange
    [Hcharged_thread
    [_Hcharged_generation
    [Hcharged_weight
    [Hcharged_slice
    [Hcharged_state
    [Hcharged_vruntime
    [_Hcharged_service
    [Hrefresh Hrq']]]]]]]]]]]]].
  pose proof Hinv as Hinv_full.
  destruct Hinv as
    [_Hshape
    [_Hinactive
    [_Hunique
    [_Hpositive
    [_Hlive
    [[Hmin_lower _Hmin_attained]
    [_Hvirtual _Hrunnable_count]]]]]]].
  pose proof (find_entity_index_active rq thread_id index old_entity Hfind Hlookup)
    as Hactive.
  assert (Hold_live : live_for_min old_entity = true).
  {
    destruct old_entity as
      [old_thread_id old_generation old_weight old_slice_ns old_service_ns
        old_vruntime old_eligible old_deadline old_state];
      destruct old_state; simpl in *; try discriminate; reflexivity.
  }
  pose proof (Hmin_lower index old_entity Hactive Hold_live) as Hfloor_old.
  pose proof (refresh_deadline_success
    charged_entity
    (er_min_vruntime rq)
    refreshed
    Hrefresh) as
    [Hrefreshed_thread
    [_Hgeneration
    [Hrefreshed_weight
    [Hrefreshed_slice
    [_Hservice
    [_Hrefreshed_vruntime
    [_Hstate_refreshed _Hdeadline]]]]]]].
  subst rq'.
  apply refresh_replace_preserves_invariant_same_thread
    with (old_entity := old_entity).
  - exact Hinv_full.
  - exact Hactive.
  - destruct old_entity as
      [old_thread_id old_generation old_weight old_slice_ns old_service_ns
        old_vruntime old_eligible old_deadline old_state];
      destruct old_state; simpl in *; try discriminate; reflexivity.
  - rewrite Hrefreshed_thread.
    exact Hcharged_thread.
  - rewrite Hrefreshed_weight.
    exact Hcharged_weight.
  - rewrite Hrefreshed_slice.
    exact Hcharged_slice.
  - intros _Hlive_new.
    apply refresh_deadline_at_vruntime_if_floor_lower
      with
        (entity := charged_entity)
        (floor := er_min_vruntime rq); auto.
    rewrite Hcharged_vruntime.
    pose proof (weighted_delta_weight_zero_fails runtime_ns) as _.
    assert (0 <= delta).
    {
      unfold weighted_delta in Hdelta.
      destruct (andb (i64_nonnegative runtime_ns)
        (valid_positive (ee_weight old_entity))) eqn:Hvalid;
        try discriminate.
      destruct ((runtime_ns * eevdf_default_weight) / ee_weight old_entity <=?
        i64_max) eqn:Hscaled; try discriminate.
      inversion Hdelta; subst delta.
      apply andb_true_iff in Hvalid as [Hruntime_nonnegative Hweight_positive].
      unfold i64_nonnegative in Hruntime_nonnegative.
      apply andb_true_iff in Hruntime_nonnegative as [Hruntime_nonnegative _].
      apply Z.leb_le in Hruntime_nonnegative.
      apply Z.ltb_lt in Hweight_positive.
      assert (0 <= runtime_ns * eevdf_default_weight).
      {
        apply Z.mul_nonneg_nonneg; unfold eevdf_default_weight; lia.
      }
      apply Z.div_pos; lia.
    }
    lia.
Qed.
