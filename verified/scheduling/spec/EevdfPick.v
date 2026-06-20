From Stdlib Require Import Bool.Bool Lia Lists.List ZArith.ZArith.
From Pacha.Scheduling Require Import EevdfModel EevdfInvariants.

Import ListNotations.
Open Scope Z_scope.

Definition eligible_at
    (virtual_time : Z)
    (entity : eevdf_entity)
  : Prop :=
  ee_state entity = ERunnable /\
  ee_eligible_time entity <= virtual_time.

Definition deadline_le
    (lhs rhs : eevdf_entity)
  : Prop :=
  ee_deadline lhs < ee_deadline rhs \/
  (ee_deadline lhs = ee_deadline rhs /\
   ee_thread_id lhs <= ee_thread_id rhs).

Definition best_eligible_spec
    (rq : eevdf_runqueue)
    (virtual_time : Z)
    (index : nat)
    (entity : eevdf_entity)
  : Prop :=
  eevdf_active_entity rq index entity /\
  eligible_at virtual_time entity /\
  forall other_index other,
    eevdf_active_entity rq other_index other ->
    eligible_at virtual_time other ->
    deadline_le entity other.

Definition no_eligible_at
    (rq : eevdf_runqueue)
    (virtual_time : Z)
  : Prop :=
  forall index entity,
    eevdf_active_entity rq index entity ->
    ~ eligible_at virtual_time entity.

Definition no_runnable
    (rq : eevdf_runqueue)
  : Prop :=
  forall index entity,
    eevdf_active_entity rq index entity ->
    ee_state entity <> ERunnable.

Definition future_runnable_time
    (rq : eevdf_runqueue)
    (time : Z)
  : Prop :=
  exists index entity,
    eevdf_active_entity rq index entity /\
    ee_state entity = ERunnable /\
    ee_eligible_time entity = time.

Definition next_eligible_spec
    (rq : eevdf_runqueue)
    (time : Z)
  : Prop :=
  future_runnable_time rq time /\
  forall index entity,
    eevdf_active_entity rq index entity ->
    ee_state entity = ERunnable ->
    time <= ee_eligible_time entity.

Definition eevdf_pick_spec
    (rq rq' : eevdf_runqueue)
    (picked : option (nat * eevdf_entity))
  : Prop :=
  match picked with
  | Some (index, entity) =>
      (rq' = rq /\ best_eligible_spec rq (er_virtual_time rq) index entity) \/
      (no_eligible_at rq (er_virtual_time rq) /\
       er_entities rq' = er_entities rq /\
       er_entity_count rq' = er_entity_count rq /\
       er_runnable_count rq' = er_runnable_count rq /\
       er_min_vruntime rq' = er_min_vruntime rq /\
       next_eligible_spec rq (er_virtual_time rq') /\
       best_eligible_spec rq' (er_virtual_time rq') index entity)
  | None =>
      rq' = rq /\
      no_eligible_at rq (er_virtual_time rq) /\
      next_eligible rq = None
  end.

Lemma eevdf_pick_current_best :
  forall rq picked,
    best_eligible rq (er_virtual_time rq) = Some picked ->
    eevdf_pick rq = (rq, Some picked).
Proof.
  intros rq picked Hbest.
  unfold eevdf_pick.
  rewrite Hbest.
  reflexivity.
Qed.

Lemma eevdf_pick_no_runnable :
  forall rq,
    best_eligible rq (er_virtual_time rq) = None ->
    next_eligible rq = None ->
    eevdf_pick rq = (rq, None).
Proof.
  intros rq Hbest Hnext.
  unfold eevdf_pick.
  rewrite Hbest.
  rewrite Hnext.
  reflexivity.
Qed.

Lemma eevdf_pick_advances_to_next_eligible :
  forall rq next_time,
    best_eligible rq (er_virtual_time rq) = None ->
    next_eligible rq = Some next_time ->
    exists rq',
      eevdf_pick rq = (rq', best_eligible rq' next_time) /\
      er_entities rq' = er_entities rq /\
      er_entity_count rq' = er_entity_count rq /\
      er_runnable_count rq' = er_runnable_count rq /\
      er_virtual_time rq' = next_time /\
      er_min_vruntime rq' = er_min_vruntime rq.
Proof.
  intros rq next_time Hbest Hnext.
  unfold eevdf_pick.
  rewrite Hbest.
  rewrite Hnext.
  eexists.
  repeat split; reflexivity.
Qed.

Definition pick_rule_result
    (rq : eevdf_runqueue)
    (virtual_time : Z)
    (picked : option (nat * eevdf_entity))
  : Prop :=
  best_eligible rq virtual_time = picked.

Definition best_pair_spec
    (items : list (nat * eevdf_entity))
    (item : nat * eevdf_entity)
  : Prop :=
  In item items /\
  forall other,
    In other items ->
    deadline_le (pair_entity item) (pair_entity other).

Lemma deadline_le_refl :
  forall entity,
    deadline_le entity entity.
Proof.
  intros entity.
  unfold deadline_le.
  right.
  split; lia.
Qed.

Lemma deadline_le_trans :
  forall lhs mid rhs,
    deadline_le lhs mid ->
    deadline_le mid rhs ->
    deadline_le lhs rhs.
Proof.
  intros lhs mid rhs Hlhs Hrhs.
  unfold deadline_le in *.
  destruct Hlhs as [Hlhs | [Hlhs_deadline Hlhs_thread]];
    destruct Hrhs as [Hrhs | [Hrhs_deadline Hrhs_thread]].
  - left. lia.
  - left. lia.
  - left. lia.
  - right. split; lia.
Qed.

Lemma entity_better_deadline_le :
  forall candidate current,
    entity_better candidate current = true ->
    deadline_le candidate current.
Proof.
  intros candidate current Hbetter.
  unfold entity_better in Hbetter.
  apply orb_true_iff in Hbetter as [Hdeadline | Htie].
  - apply Z.ltb_lt in Hdeadline.
    unfold deadline_le.
    left. exact Hdeadline.
  - apply andb_true_iff in Htie as [Hdeadline Hthread].
    apply Z.eqb_eq in Hdeadline.
    apply Z.ltb_lt in Hthread.
    unfold deadline_le.
    right. split; lia.
Qed.

Lemma entity_not_better_deadline_le :
  forall candidate current,
    entity_better candidate current = false ->
    deadline_le current candidate.
Proof.
  intros candidate current Hbetter.
  unfold entity_better in Hbetter.
  destruct (ee_deadline candidate <? ee_deadline current) eqn:Hdeadline_lt;
    try discriminate.
  apply Z.ltb_ge in Hdeadline_lt.
  unfold deadline_le.
  destruct (ee_deadline candidate =? ee_deadline current) eqn:Hdeadline_eq.
  - apply Z.eqb_eq in Hdeadline_eq.
    destruct (ee_thread_id candidate <? ee_thread_id current) eqn:Hthread_lt;
      try discriminate.
    apply Z.ltb_ge in Hthread_lt.
      right. split; lia.
  - apply Z.eqb_neq in Hdeadline_eq.
    left. lia.
Qed.

Lemma select_best_none_empty :
  forall items,
    select_best items = None ->
    items = [].
Proof.
  intros items.
  destruct items as [| item rest]; simpl; auto.
  destruct (select_best rest) as [best |] eqn:Hrest; intros Hbest.
  - destruct (pair_better item best); inversion Hbest.
  - inversion Hbest.
Qed.

Lemma select_best_some_spec :
  forall items item,
    select_best items = Some item ->
    best_pair_spec items item.
Proof.
  induction items as [| head rest IH]; intros item Hbest; simpl in Hbest;
    try discriminate.
  set (rest_pick := select_best rest) in Hbest.
  assert (Hrest_pick_def : select_best rest = rest_pick) by reflexivity.
  destruct rest_pick as [rest_best |].
  - pose proof (IH rest_best Hrest_pick_def) as [Hrest_in Hrest_min].
    destruct (pair_better head rest_best) eqn:Hbetter.
    + inversion Hbest; subst item.
      split.
      * left. reflexivity.
      * intros other Hother.
        destruct Hother as [Hhead | Hother].
        -- subst other. apply deadline_le_refl.
        -- eapply deadline_le_trans.
           ++ apply entity_better_deadline_le. exact Hbetter.
           ++ apply Hrest_min. exact Hother.
    + inversion Hbest; subst item.
      split.
      * right. exact Hrest_in.
      * intros other Hother.
        destruct Hother as [Hhead | Hother].
        -- subst other.
           apply entity_not_better_deadline_le. exact Hbetter.
        -- apply Hrest_min. exact Hother.
  - apply select_best_none_empty in Hrest_pick_def.
    subst rest.
    inversion Hbest; subst item.
    split.
    + left. reflexivity.
    + intros other [Hother | []].
      subst other.
      apply deadline_le_refl.
Qed.

Lemma active_indexed_from_in :
  forall entities remaining base offset entity,
    (offset < remaining)%nat ->
    nth_error entities offset = Some entity ->
    In (base + offset, entity)%nat
      (active_indexed_from entities remaining base).
Proof.
  induction entities as [| head rest IH];
    intros remaining base offset entity Hlt Hlookup.
  - destruct remaining as [| remaining']; simpl in *.
    + lia.
    + destruct offset; discriminate.
  - destruct remaining as [| remaining']; simpl in *.
    + lia.
    + destruct offset as [| offset'].
      * inversion Hlookup; subst; simpl.
        left.
        replace (base + 0)%nat with base by lia.
        reflexivity.
      * right.
        replace (base + S offset')%nat with (S base + offset')%nat by lia.
        apply IH; auto; lia.
Qed.

Lemma active_indexed_from_in_inv :
  forall entities remaining base item,
    In item (active_indexed_from entities remaining base) ->
    exists offset entity,
      item = (base + offset, entity)%nat /\
      (offset < remaining)%nat /\
      nth_error entities offset = Some entity.
Proof.
  induction entities as [| head rest IH];
    intros remaining base item Hin.
  - destruct remaining as [| remaining']; simpl in *; contradiction.
  - destruct remaining as [| remaining']; simpl in *; try contradiction.
    destruct Hin as [Hin | Hin].
    + subst item.
      exists 0%nat.
      exists head.
      split.
      * replace (base + 0)%nat with base by lia.
        reflexivity.
      * split; simpl; [lia | reflexivity].
    + apply IH in Hin as [offset [entity [Hitem [Hlt Hlookup]]]].
      exists (S offset).
      exists entity.
      split.
      * rewrite Hitem.
        replace (base + S offset)%nat with (S base + offset)%nat by lia.
        reflexivity.
      * split; simpl; auto; lia.
Qed.

Lemma eligible_indexed_in :
  forall virtual_time items item,
    In item (eligible_indexed virtual_time items) ->
    In item items /\ eligible_at virtual_time (pair_entity item).
Proof.
  induction items as [| head rest IH]; intros item Hin; simpl in Hin;
    try contradiction.
  destruct (eligible_pair virtual_time head) eqn:Heligible.
  - destruct Hin as [Hin | Hin].
    + subst item.
      split; [left; reflexivity |].
      unfold eligible_pair in Heligible.
      destruct head as [index entity].
      unfold pair_entity.
      apply andb_true_iff in Heligible as [Hrunnable Heligible_time].
      unfold is_runnable in Hrunnable.
      destruct (ee_state entity) eqn:Hstate; simpl in Hrunnable; try discriminate.
      apply Z.leb_le in Heligible_time.
      split; [exact Hstate | exact Heligible_time].
    + apply IH in Hin as [Hin Hel].
      split; [right; exact Hin | exact Hel].
  - apply IH in Hin as [Hin Hel].
    split; [right; exact Hin | exact Hel].
Qed.

Lemma eligible_indexed_contains :
  forall virtual_time items item,
    In item items ->
    eligible_at virtual_time (pair_entity item) ->
    In item (eligible_indexed virtual_time items).
Proof.
  induction items as [| head rest IH]; intros item Hin Heligible;
    simpl in *; try contradiction.
  destruct Hin as [Hin | Hin].
  - subst item.
    destruct head as [index entity].
    simpl in Heligible.
    destruct Heligible as [Hrunnable Heligible_time].
    unfold eligible_pair.
    simpl.
    unfold is_runnable.
    rewrite Hrunnable.
    apply Z.leb_le in Heligible_time.
    rewrite Heligible_time.
    left. reflexivity.
  - destruct (eligible_pair virtual_time head); simpl.
    + right. apply IH; auto.
    + apply IH; auto.
Qed.

Lemma select_best_some_exists :
  forall items item,
    In item items ->
    exists best,
      select_best items = Some best.
Proof.
  induction items as [| head rest IH]; intros item Hin; simpl in *;
    try contradiction.
  destruct (select_best rest) as [rest_best |] eqn:Hrest_best.
  - destruct (pair_better head rest_best);
      eexists; reflexivity.
  - exists head.
    reflexivity.
Qed.

Definition earliest_pair_spec
    (items : list (nat * eevdf_entity))
    (item : nat * eevdf_entity)
  : Prop :=
  In item items /\
  forall other,
    In other items ->
    pair_eligible_time item <= pair_eligible_time other.

Lemma pair_earlier_time_le :
  forall candidate current,
    pair_earlier candidate current = true ->
    pair_eligible_time candidate <= pair_eligible_time current.
Proof.
  intros candidate current Hearlier.
  unfold pair_earlier in Hearlier.
  apply Z.ltb_lt in Hearlier.
  lia.
Qed.

Lemma pair_not_earlier_time_le :
  forall candidate current,
    pair_earlier candidate current = false ->
    pair_eligible_time current <= pair_eligible_time candidate.
Proof.
  intros candidate current Hearlier.
  unfold pair_earlier in Hearlier.
  apply Z.ltb_ge in Hearlier.
  exact Hearlier.
Qed.

Lemma select_earliest_none_empty :
  forall items,
    select_earliest items = None ->
    items = [].
Proof.
  intros items.
  destruct items as [| item rest]; simpl; auto.
  destruct (select_earliest rest) as [earliest |] eqn:Hrest; intros Hselect.
  - destruct (pair_earlier item earliest); inversion Hselect.
  - inversion Hselect.
Qed.

Lemma select_earliest_some_spec :
  forall items item,
    select_earliest items = Some item ->
    earliest_pair_spec items item.
Proof.
  induction items as [| head rest IH]; intros item Hselect; simpl in Hselect;
    try discriminate.
  set (rest_pick := select_earliest rest) in Hselect.
  assert (Hrest_pick_def : select_earliest rest = rest_pick) by reflexivity.
  destruct rest_pick as [rest_best |].
  - pose proof (IH rest_best Hrest_pick_def) as [Hrest_in Hrest_min].
    destruct (pair_earlier head rest_best) eqn:Hearlier.
    + inversion Hselect; subst item.
      split.
      * left. reflexivity.
      * intros other Hother.
        destruct Hother as [Hhead | Hother].
        -- subst other. lia.
        -- pose proof (pair_earlier_time_le head rest_best Hearlier) as Hhead_le.
           pose proof (Hrest_min other Hother) as Hrest_le.
           lia.
    + inversion Hselect; subst item.
      split.
      * right. exact Hrest_in.
      * intros other Hother.
        destruct Hother as [Hhead | Hother].
        -- subst other.
           apply pair_not_earlier_time_le. exact Hearlier.
        -- apply Hrest_min. exact Hother.
  - apply select_earliest_none_empty in Hrest_pick_def.
    subst rest.
    inversion Hselect; subst item.
    split.
    + left. reflexivity.
    + intros other [Hother | []].
      subst other.
      lia.
Qed.

Lemma select_earliest_some_exists :
  forall items item,
    In item items ->
    exists earliest,
      select_earliest items = Some earliest.
Proof.
  induction items as [| head rest IH]; intros item Hin; simpl in *;
    try contradiction.
  destruct (select_earliest rest) as [rest_best |] eqn:Hrest_best.
  - destruct (pair_earlier head rest_best);
      eexists; reflexivity.
  - exists head.
    reflexivity.
Qed.

Lemma runnable_pair_state :
  forall item,
    runnable_pair item = true ->
    ee_state (pair_entity item) = ERunnable.
Proof.
  intros [index entity] Hrunnable.
  unfold runnable_pair, pair_entity, is_runnable in Hrunnable.
  simpl in *.
  destruct (ee_state entity) eqn:Hstate; try discriminate.
  reflexivity.
Qed.

Lemma runnable_indexed_in :
  forall items item,
    In item (runnable_indexed items) ->
    In item items /\ ee_state (pair_entity item) = ERunnable.
Proof.
  induction items as [| head rest IH]; intros item Hin; simpl in Hin;
    try contradiction.
  destruct (runnable_pair head) eqn:Hrunnable.
  - destruct Hin as [Hin | Hin].
    + subst item.
      split; [left; reflexivity |].
      apply runnable_pair_state.
      exact Hrunnable.
    + apply IH in Hin as [Hin Hstate].
      split; [right; exact Hin | exact Hstate].
  - apply IH in Hin as [Hin Hstate].
    split; [right; exact Hin | exact Hstate].
Qed.

Lemma runnable_indexed_contains :
  forall items item,
    In item items ->
    ee_state (pair_entity item) = ERunnable ->
    In item (runnable_indexed items).
Proof.
  induction items as [| head rest IH]; intros item Hin Hstate;
    simpl in *; try contradiction.
  destruct Hin as [Hin | Hin].
  - subst item.
    unfold runnable_pair, is_runnable.
    destruct head as [index entity].
    simpl in *.
    rewrite Hstate.
    left. reflexivity.
  - destruct (runnable_pair head); simpl.
    + right. apply IH; auto.
    + apply IH; auto.
Qed.

Theorem best_eligible_some_spec :
  forall rq virtual_time index entity,
    best_eligible rq virtual_time = Some (index, entity) ->
    best_eligible_spec rq virtual_time index entity.
Proof.
  intros rq virtual_time index entity Hbest.
  unfold best_eligible, best_eligible_from in Hbest.
  pose proof (select_best_some_spec
    (eligible_indexed virtual_time
      (active_indexed_from (er_entities rq) (er_entity_count rq) 0))
    (index, entity)
    Hbest) as [Hin_selected Hmin].
  apply eligible_indexed_in in Hin_selected as [Hin_active Heligible].
  apply active_indexed_from_in_inv in Hin_active as
    [offset [active_entity [Hpair [Hoffset Hlookup]]]].
  inversion Hpair; subst.
  simpl in *.
  replace (0 + offset)%nat with offset in * by lia.
  split.
  - split; auto.
  - split; [exact Heligible |].
    intros other_index other Hactive_other Heligible_other.
    destruct Hactive_other as [Hother_lt Hother_lookup].
    pose proof (active_indexed_from_in
      (er_entities rq)
      (er_entity_count rq)
      0
      other_index
      other
      Hother_lt
      Hother_lookup) as Hother_active_in.
    replace (0 + other_index)%nat with other_index in Hother_active_in by lia.
    pose proof (eligible_indexed_contains
      virtual_time
      (active_indexed_from (er_entities rq) (er_entity_count rq) 0)
      (other_index, other)
      Hother_active_in
      Heligible_other) as Hother_eligible_in.
    specialize (Hmin (other_index, other) Hother_eligible_in).
    exact Hmin.
Qed.

Theorem best_eligible_exists_if_eligible :
  forall rq virtual_time index entity,
    eevdf_active_entity rq index entity ->
    eligible_at virtual_time entity ->
    exists picked,
      best_eligible rq virtual_time = Some picked.
Proof.
  intros rq virtual_time index entity [Hindex Hlookup] Heligible.
  unfold best_eligible, best_eligible_from.
  pose proof (active_indexed_from_in
    (er_entities rq)
    (er_entity_count rq)
    0
    index
    entity
    Hindex
    Hlookup) as Hactive_in.
  replace (0 + index)%nat with index in Hactive_in by lia.
  pose proof (eligible_indexed_contains
    virtual_time
    (active_indexed_from (er_entities rq) (er_entity_count rq) 0)
    (index, entity)
    Hactive_in
    Heligible) as Heligible_in.
  apply select_best_some_exists with (item := (index, entity)).
  exact Heligible_in.
Qed.

Theorem best_eligible_none_no_eligible :
  forall rq virtual_time,
    best_eligible rq virtual_time = None ->
    no_eligible_at rq virtual_time.
Proof.
  intros rq virtual_time Hbest index entity Hactive Heligible.
  destruct (best_eligible_exists_if_eligible
    rq
    virtual_time
    index
    entity
    Hactive
    Heligible) as [picked Hpicked].
  rewrite Hbest in Hpicked.
  discriminate.
Qed.

Theorem next_eligible_some_spec :
  forall rq time,
    next_eligible rq = Some time ->
    next_eligible_spec rq time.
Proof.
  intros rq time Hnext.
  unfold next_eligible in Hnext.
  set (active :=
    active_indexed_from (er_entities rq) (er_entity_count rq) 0%nat) in Hnext.
  set (runnable := runnable_indexed active) in Hnext.
  assert (Hactive_def :
    active =
      active_indexed_from (er_entities rq) (er_entity_count rq) 0%nat)
    by reflexivity.
  assert (Hrunnable_def : runnable = runnable_indexed active) by reflexivity.
  destruct (select_earliest runnable) as [[picked_index picked_entity] |]
    eqn:Hselect; try discriminate.
  inversion Hnext; subst time.
  pose proof (select_earliest_some_spec
    runnable
    (picked_index, picked_entity)
    Hselect) as [Hin_runnable Hmin].
  apply runnable_indexed_in in Hin_runnable as [Hin_active Hpicked_state].
  rewrite Hactive_def in Hin_active.
  apply active_indexed_from_in_inv in Hin_active as
    [offset [active_entity [Hpair [Hoffset Hlookup]]]].
  inversion Hpair; subst.
  simpl in *.
  replace (0 + offset)%nat with offset in * by lia.
  split.
  - exists offset.
    exists active_entity.
    repeat split; auto.
  - intros index entity Hactive_entity Hrunnable_state.
    destruct Hactive_entity as [Hindex Hlookup_entity].
    pose proof (active_indexed_from_in
      (er_entities rq)
      (er_entity_count rq)
      0
      index
      entity
      Hindex
      Hlookup_entity) as Hin_active_other.
    replace (0 + index)%nat with index in Hin_active_other by lia.
    assert (Hin_runnable_other :
      In (index, entity) runnable).
    {
      rewrite Hrunnable_def.
      apply runnable_indexed_contains.
      - rewrite Hactive_def.
        exact Hin_active_other.
      - simpl.
        exact Hrunnable_state.
    }
    specialize (Hmin (index, entity) Hin_runnable_other).
    simpl in Hmin.
    exact Hmin.
Qed.

Theorem next_eligible_none_no_future_runnable :
  forall rq,
    next_eligible rq = None ->
    forall index entity,
      eevdf_active_entity rq index entity ->
      ee_state entity = ERunnable ->
      False.
Proof.
  intros rq Hnext index entity [Hindex Hlookup] Hrunnable_state.
  unfold next_eligible in Hnext.
  pose proof (active_indexed_from_in
    (er_entities rq)
    (er_entity_count rq)
    0
    index
    entity
    Hindex
    Hlookup) as Hin_active.
  replace (0 + index)%nat with index in Hin_active by lia.
  pose proof (runnable_indexed_contains
    (active_indexed_from (er_entities rq) (er_entity_count rq) 0)
    (index, entity)
    Hin_active
    Hrunnable_state) as Hin_runnable.
  destruct (select_earliest_some_exists
    (runnable_indexed
      (active_indexed_from (er_entities rq) (er_entity_count rq) 0))
    (index, entity)
    Hin_runnable) as [earliest Hearliest].
  rewrite Hearliest in Hnext.
  destruct earliest as [earliest_index earliest_entity].
  discriminate.
Qed.

Theorem no_runnable_next_eligible_none :
  forall rq,
    no_runnable rq ->
    next_eligible rq = None.
Proof.
  intros rq Hno_runnable.
  destruct (next_eligible rq) as [next_time |] eqn:Hnext;
    try reflexivity.
  pose proof (next_eligible_some_spec rq next_time Hnext)
    as [[index [entity [Hactive [Hstate _]]]] _].
  specialize (Hno_runnable index entity Hactive).
  contradiction.
Qed.

Theorem no_runnable_no_eligible :
  forall rq virtual_time,
    no_runnable rq ->
    no_eligible_at rq virtual_time.
Proof.
  intros rq virtual_time Hno_runnable index entity Hactive Heligible.
  destruct Heligible as [Hstate _].
  specialize (Hno_runnable index entity Hactive).
  contradiction.
Qed.

Theorem eevdf_pick_none_if_no_runnable :
  forall rq,
    no_runnable rq ->
    eevdf_pick rq = (rq, None).
Proof.
  intros rq Hno_runnable.
  assert (Hno_eligible : no_eligible_at rq (er_virtual_time rq)).
  {
    apply no_runnable_no_eligible.
    exact Hno_runnable.
  }
  destruct (best_eligible rq (er_virtual_time rq)) as [[index entity] |]
    eqn:Hbest.
  - pose proof (best_eligible_some_spec
      rq
      (er_virtual_time rq)
      index
      entity
      Hbest) as [Hactive [Heligible _]].
    exact (False_rect _ (Hno_eligible index entity Hactive Heligible)).
  - apply eevdf_pick_no_runnable.
    + exact Hbest.
    + apply no_runnable_next_eligible_none.
      exact Hno_runnable.
Qed.

Theorem eevdf_pick_returns_min_if_eligible :
  forall rq index entity,
    eevdf_active_entity rq index entity ->
    eligible_at (er_virtual_time rq) entity ->
    exists picked_index picked_entity,
      eevdf_pick rq = (rq, Some (picked_index, picked_entity)) /\
      best_eligible_spec rq (er_virtual_time rq) picked_index picked_entity.
Proof.
  intros rq index entity Hactive Heligible.
  destruct (best_eligible_exists_if_eligible
    rq
    (er_virtual_time rq)
    index
    entity
    Hactive
    Heligible) as [[picked_index picked_entity] Hbest].
  exists picked_index.
  exists picked_entity.
  split.
  - apply eevdf_pick_current_best.
    exact Hbest.
  - apply best_eligible_some_spec.
    exact Hbest.
Qed.

Theorem eevdf_pick_advances_to_min_next_eligible :
  forall rq next_time,
    best_eligible rq (er_virtual_time rq) = None ->
    next_eligible rq = Some next_time ->
    exists rq' picked_index picked_entity,
      eevdf_pick rq = (rq', Some (picked_index, picked_entity)) /\
      er_entities rq' = er_entities rq /\
      er_entity_count rq' = er_entity_count rq /\
      er_runnable_count rq' = er_runnable_count rq /\
      er_virtual_time rq' = next_time /\
      er_min_vruntime rq' = er_min_vruntime rq /\
      no_eligible_at rq (er_virtual_time rq) /\
      next_eligible_spec rq next_time /\
      best_eligible_spec rq' next_time picked_index picked_entity.
Proof.
  intros rq next_time Hbest Hnext.
  pose proof (next_eligible_some_spec rq next_time Hnext) as Hnext_spec.
  pose proof Hnext_spec as Hnext_spec_parts.
  destruct Hnext_spec_parts as
    [[future_index [future_entity [Hfuture_active [Hfuture_state Hfuture_time]]]]
      Hnext_min].
  set (rq' :=
    {|
      er_entities := er_entities rq;
      er_entity_count := er_entity_count rq;
      er_runnable_count := er_runnable_count rq;
      er_virtual_time := next_time;
      er_min_vruntime := er_min_vruntime rq;
    |}).
  assert (Hfuture_active_rq' : eevdf_active_entity rq' future_index future_entity).
  {
    subst rq'.
    exact Hfuture_active.
  }
  assert (Hfuture_eligible_rq' : eligible_at next_time future_entity).
  {
    split; auto; lia.
  }
  destruct (best_eligible_exists_if_eligible
    rq'
    next_time
    future_index
    future_entity
    Hfuture_active_rq'
    Hfuture_eligible_rq') as [[picked_index picked_entity] Hpicked].
  exists rq'.
  exists picked_index.
  exists picked_entity.
  subst rq'.
  split.
  - unfold eevdf_pick.
    rewrite Hbest.
    rewrite Hnext.
    rewrite Hpicked.
    reflexivity.
  - split; [reflexivity |].
    split; [reflexivity |].
    split; [reflexivity |].
    split; [reflexivity |].
    split; [reflexivity |].
    split.
    + apply best_eligible_none_no_eligible.
      exact Hbest.
    + split.
      * exact Hnext_spec.
      * apply best_eligible_some_spec.
        exact Hpicked.
Qed.

Theorem eevdf_pick_uses_advanced_pick_rule :
  forall rq rq' picked,
    eevdf_pick rq = (rq', picked) ->
    rq' <> rq ->
    exists next_time,
      best_eligible rq (er_virtual_time rq) = None /\
      next_eligible rq = Some next_time /\
      er_virtual_time rq' = next_time /\
      pick_rule_result rq' next_time picked.
Proof.
  intros rq rq' picked Hpick Hadvanced.
  unfold eevdf_pick in Hpick.
  destruct (best_eligible rq (er_virtual_time rq)) as [[best_index best_entity] |]
    eqn:Hbest.
  - inversion Hpick; subst.
    contradiction.
  - destruct (next_eligible rq) as [next_time |] eqn:Hnext.
    + inversion Hpick; subst.
      exists next_time.
      repeat split; auto.
    + inversion Hpick; subst.
      contradiction.
Qed.
