From Stdlib Require Import Arith.PeanoNat Bool.Bool Lia Lists.List ZArith.ZArith.
From Pacha.Scheduling Require Import
  ProtocolModel
  EevdfModel
  EevdfInvariants
  EevdfCharge
  EevdfPick
  EevdfTransitions
  EevdfPreservation
  KernelSchedModel
  KernelSchedInvariants.

Import ListNotations.
Open Scope Z_scope.

Definition eevdf_invariant_without_thread_uniqueness
    (rq : eevdf_runqueue)
  : Prop :=
  eevdf_shape rq /\
  eevdf_inactive_entities_empty rq /\
  eevdf_positive_entity_params rq /\
  eevdf_live_deadlines_consistent rq /\
  eevdf_min_vruntime_consistent rq /\
  eevdf_virtual_time_consistent rq /\
  eevdf_runnable_count_consistent rq.

Definition kernel_runqueues_invariant_without_thread_uniqueness
    (sched : kernel_sched_state)
  : Prop :=
  forall cpu_id rq,
    kernel_active_runqueue sched cpu_id rq ->
    eevdf_invariant_without_thread_uniqueness rq.

Lemma eevdf_invariant_implies_without_thread_uniqueness :
  forall rq,
    eevdf_invariant rq ->
    eevdf_invariant_without_thread_uniqueness rq.
Proof.
  intros rq Hinv.
  destruct Hinv as
    [Hshape
    [Hinactive
    [_Hunique
    [Hpositive
    [Hlive
    [Hmin
    [Hvirtual Hrunnable]]]]]]].
  unfold eevdf_invariant_without_thread_uniqueness.
  split; [exact Hshape |].
  split; [exact Hinactive |].
  split; [exact Hpositive |].
  split; [exact Hlive |].
  split; [exact Hmin |].
  split; [exact Hvirtual |].
  exact Hrunnable.
Qed.

Lemma nth_error_repeat_some :
  forall {A : Type} (item value : A) count index,
    nth_error (repeat item count) index = Some value ->
    value = item.
Proof.
  intros A item value count.
  induction count as [| count IH]; intros index Hlookup.
  - destruct index; simpl in Hlookup; discriminate.
  - destruct index as [| index']; simpl in Hlookup.
    + inversion Hlookup; reflexivity.
    + apply IH with (index := index'); exact Hlookup.
Qed.

Lemma replace_nth_same :
  forall {A : Type} (items : list A) index item,
    nth_error items index = Some item ->
    replace_nth items index item = items.
Proof.
  induction items as [| head rest IH]; intros index item Hlookup.
  - destruct index; discriminate.
  - destruct index as [| index']; simpl in *.
    + inversion Hlookup; subst item.
      reflexivity.
    + rewrite IH with (item := item); auto.
Qed.

Lemma filter_length_le :
  forall {A : Type} (predicate : A -> bool) (items : list A),
    (length (filter predicate items) <= length items)%nat.
Proof.
  intros A predicate items.
  induction items as [| item rest IH]; simpl.
  - lia.
  - destruct (predicate item); simpl; lia.
Qed.

Lemma filter_nth_error_source :
  forall {A : Type} (predicate : A -> bool) items index item,
    nth_error (filter predicate items) index = Some item ->
    exists source_index,
      nth_error items source_index = Some item.
Proof.
  intros A predicate items.
  induction items as [| head rest IH]; intros index item Hlookup.
  - destruct index; simpl in Hlookup; discriminate.
  - simpl in Hlookup.
    destruct (predicate head) eqn:Hhead.
    + destruct index as [| index'].
      * inversion Hlookup; subst item.
        exists 0%nat.
        reflexivity.
      * destruct (IH index' item Hlookup) as [source_index Hsource].
        exists (S source_index).
        exact Hsource.
    + destruct (IH index item Hlookup) as [source_index Hsource].
      exists (S source_index).
      exact Hsource.
Qed.

Definition list_active_thread_ids_unique
    (items : list eevdf_entity)
  : Prop :=
  forall i j lhs rhs,
    nth_error items i = Some lhs ->
    nth_error items j = Some rhs ->
    ee_thread_id lhs <> no_thread_id ->
    ee_thread_id lhs = ee_thread_id rhs ->
    is_active_state (ee_state lhs) = true ->
    is_active_state (ee_state rhs) = true ->
    i = j.

Lemma list_active_thread_ids_unique_tail :
  forall head rest,
    list_active_thread_ids_unique (head :: rest) ->
    list_active_thread_ids_unique rest.
Proof.
  intros head rest Hunique.
  unfold list_active_thread_ids_unique in *.
  intros i j lhs rhs Hlhs Hrhs Hlhs_nonzero Hthreads_eq
    Hlhs_active Hrhs_active.
  assert (Hsame : S i = S j).
  {
    eapply Hunique; eauto.
  }
  inversion Hsame.
  reflexivity.
Qed.

Lemma list_active_thread_ids_unique_filter :
  forall predicate items,
    list_active_thread_ids_unique items ->
    list_active_thread_ids_unique (filter predicate items).
Proof.
  intros predicate items.
  induction items as [| head rest IH]; intros Hunique.
  - unfold list_active_thread_ids_unique.
    intros i j lhs rhs Hlhs _Hrhs _Hnonzero _Heq _Hlhs_active _Hrhs_active.
    destruct i; simpl in Hlhs; discriminate.
  - simpl.
    destruct (predicate head) eqn:Hhead.
    + unfold list_active_thread_ids_unique.
      intros i j lhs rhs Hlhs Hrhs Hlhs_nonzero Hthreads_eq
        Hlhs_active Hrhs_active.
      destruct i as [| i']; destruct j as [| j']; simpl in *.
      * reflexivity.
      * inversion Hlhs; subst lhs.
        destruct (filter_nth_error_source predicate rest j' rhs Hrhs)
          as [source_j Hsource_j].
        assert (Hsame : 0%nat = S source_j).
        {
          eapply Hunique; eauto.
        }
        discriminate.
      * inversion Hrhs; subst rhs.
        destruct (filter_nth_error_source predicate rest i' lhs Hlhs)
          as [source_i Hsource_i].
        assert (Hsame : S source_i = 0%nat).
        {
          eapply Hunique; eauto.
        }
        discriminate.
      * assert (Htail_unique : list_active_thread_ids_unique rest).
        {
          apply list_active_thread_ids_unique_tail with (head := head).
          exact Hunique.
        }
        apply IH in Htail_unique.
        unfold list_active_thread_ids_unique in Htail_unique.
        f_equal.
        eapply Htail_unique; eauto.
    + apply IH.
      apply list_active_thread_ids_unique_tail with (head := head).
      exact Hunique.
Qed.

Lemma find_entity_index_from_some_match :
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

Lemma find_entity_index_lookup_matches_thread :
  forall rq thread_id index entity,
    find_entity_index rq thread_id = Some index ->
    lookup_entity rq index = Some entity ->
    ee_thread_id entity = thread_id /\
    is_active_state (ee_state entity) = true.
Proof.
  intros rq thread_id index entity Hfind Hlookup.
  unfold find_entity_index in Hfind.
  destruct (thread_id =? no_thread_id); try discriminate.
  destruct (find_entity_index_from_some_match
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

Lemma find_entity_index_thread_not_none :
  forall rq thread_id index,
    find_entity_index rq thread_id = Some index ->
    thread_id <> no_thread_id.
Proof.
  intros rq thread_id index Hfind Hnone.
  subst thread_id.
  unfold find_entity_index in Hfind.
  rewrite Z.eqb_refl in Hfind.
  discriminate.
Qed.

Lemma kernel_empty_runqueue_invariant :
  eevdf_invariant eevdf_empty_runqueue.
Proof.
  pose proof (reset_preserves_invariant eevdf_empty_runqueue) as Hreset.
  simpl in Hreset.
  exact Hreset.
Qed.

Lemma kernel_empty_active_entity_impossible :
  forall index entity,
    eevdf_active_entity eevdf_empty_runqueue index entity ->
    False.
Proof.
  intros index entity [Hlt _].
  unfold eevdf_empty_runqueue in Hlt.
  simpl in Hlt.
  lia.
Qed.

Theorem kernel_empty_state_preserves_invariant :
  forall cpu_count,
    kernel_sched_invariant (kernel_sched_empty_state cpu_count).
Proof.
  intros cpu_count.
  unfold kernel_sched_invariant.
  split.
  - unfold kernel_sched_shape, kernel_sched_empty_state.
    split.
    + simpl.
      destruct (kernel_sched_max_cpus <? cpu_count)%nat eqn:Hlt;
        try lia.
      apply Nat.ltb_ge in Hlt.
      lia.
    + split; simpl; reflexivity.
  - split.
    + unfold kernel_runqueues_invariant.
      intros cpu_idx local_rq [_ Hlookup].
      apply nth_error_repeat_some in Hlookup.
      subst local_rq.
      exact kernel_empty_runqueue_invariant.
    + split.
      * unfold kernel_global_thread_ids_unique.
        intros cpu_a index_a cpu_b index_b lhs rhs
          [rq_a [[_ Hlookup_a] [Hactive_a _]]] _ _ _.
        apply nth_error_repeat_some in Hlookup_a.
        subst rq_a.
        apply kernel_empty_active_entity_impossible in Hactive_a.
        contradiction.
      * split.
        -- unfold kernel_current_matches_local_running.
           intros cpu_idx cpu [_ Hlookup] Hcurrent.
           apply nth_error_repeat_some in Hlookup.
           subst cpu.
           simpl in Hcurrent.
           discriminate.
        -- split.
           ++ unfold kernel_running_entity_has_current.
              intros cpu_idx local_rq index entity [_ Hlookup] Hactive Hrunning.
              apply nth_error_repeat_some in Hlookup.
              subst local_rq.
              apply kernel_empty_active_entity_impossible in Hactive.
              contradiction.
           ++ split.
              ** unfold kernel_no_cross_cpu_current_duplicates.
                 intros cpu_a cpu_b lhs rhs [_ Hlookup_lhs] _ Hcurrent_lhs _ _ _.
                 apply nth_error_repeat_some in Hlookup_lhs.
                 subst lhs.
                 simpl in Hcurrent_lhs.
                 discriminate.
              ** unfold kernel_activation_targets_valid_cpus.
                 intros cpu_idx cpu Hlookup Hpending.
                 apply nth_error_repeat_some in Hlookup.
                 subst cpu.
                 simpl in Hpending.
                 discriminate.
Qed.

Lemma kernel_valid_cpu_lt :
  forall sched cpu_idx,
    kernel_valid_cpu sched cpu_idx = true ->
    (cpu_idx < ks_cpu_count sched)%nat.
Proof.
  intros sched cpu_idx Hvalid.
  unfold kernel_valid_cpu in Hvalid.
  apply Nat.ltb_lt.
  exact Hvalid.
Qed.

Lemma kernel_lookup_cpu_some :
  forall sched cpu_idx,
    kernel_sched_shape sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    exists cpu,
      kernel_lookup_cpu sched cpu_idx = Some cpu.
Proof.
  intros sched cpu_idx [Hcount [_ Hcpus_len]] Hvalid.
  apply kernel_valid_cpu_lt in Hvalid.
  unfold kernel_lookup_cpu.
  destruct (nth_error (ks_cpus sched) cpu_idx) as [cpu |] eqn:Hlookup.
  - exists cpu. reflexivity.
  - apply nth_error_None in Hlookup.
    lia.
Qed.

Lemma kernel_cpu_current_slot_matches :
  forall sched cpu_idx cpu thread_id generation,
    kernel_cpu_current sched cpu_idx = Some (thread_id, generation) ->
    kernel_cpu_slot sched cpu_idx cpu ->
    kc_has_current cpu = true /\
    kc_current_thread_id cpu = thread_id /\
    kc_current_generation cpu = generation.
Proof.
  intros sched cpu_idx cpu thread_id generation Hcurrent Hslot.
  unfold kernel_cpu_current in Hcurrent.
  destruct (kernel_cpu_has_current sched cpu_idx) eqn:Hhas; try discriminate.
  unfold kernel_cpu_has_current in Hhas.
  destruct (kernel_valid_cpu sched cpu_idx) eqn:Hvalid; try discriminate.
  destruct (kernel_lookup_cpu sched cpu_idx) as [looked_cpu |] eqn:Hlookup;
    try discriminate.
  destruct Hslot as [_ Hslot_lookup].
  unfold kernel_lookup_cpu in Hlookup.
  rewrite Hslot_lookup in Hlookup.
  inversion Hlookup; subst looked_cpu.
  inversion Hcurrent; subst thread_id generation.
  split.
  - exact Hhas.
  - split; reflexivity.
Qed.

Lemma kernel_current_entity_index_from_finds_running :
  forall entities remaining thread_id generation base offset entity,
    nth_error entities offset = Some entity ->
    (offset < remaining)%nat ->
    ee_thread_id entity = thread_id ->
    ee_generation entity = generation ->
    ee_state entity = ERunning ->
    exists found,
      kernel_current_entity_index_from
        entities
        remaining
        thread_id
        generation
        base =
      Some found.
Proof.
  induction entities as [| head rest IH];
    intros remaining thread_id generation base offset entity
      Hlookup Hlt Hthread Hgeneration Hstate.
  - destruct offset; simpl in Hlookup; discriminate.
  - destruct remaining as [| remaining']; simpl in *.
    + lia.
    + destruct offset as [| offset'].
      * inversion Hlookup; subst entity.
        simpl.
        replace (ee_thread_id head =? thread_id) with true.
        -- replace (ee_generation head =? generation) with true.
           ++ rewrite Hstate.
              simpl.
              exists base.
              reflexivity.
           ++ symmetry.
              apply Z.eqb_eq.
              exact Hgeneration.
        -- symmetry.
           apply Z.eqb_eq.
           exact Hthread.
      * destruct
          (andb
            (andb
              (ee_thread_id head =? thread_id)
              (ee_generation head =? generation))
            match ee_state head with
            | ERunning => true
            | _ => false
            end) eqn:Hhead.
        -- exists base.
           reflexivity.
        -- apply IH with
             (offset := offset')
             (entity := entity);
             auto;
             lia.
Qed.

Theorem kernel_current_entity_index_exists_from_invariant :
  forall sched cpu_idx cpu rq thread_id generation,
    kernel_sched_invariant sched ->
    kernel_cpu_slot sched cpu_idx cpu ->
    kc_has_current cpu = true ->
    kc_current_thread_id cpu = thread_id ->
    kc_current_generation cpu = generation ->
    kernel_active_runqueue sched cpu_idx rq ->
    exists index,
      kernel_current_entity_index rq thread_id generation = Some index.
Proof.
  intros sched cpu_idx cpu rq thread_id generation
    Hinv Hslot Hhas Hthread_current Hgeneration_current Hactive_rq.
  destruct Hinv as
    [_Hshape
      [_Hrqs
      [_Hunique
      [Hcurrent_matches
      [_Hrunning
      [_Hnodup _Htargets]]]]]].
  destruct (Hcurrent_matches
    cpu_idx
    cpu
    Hslot
    Hhas) as
    [current_rq [current_index [current_entity
      [Hcurrent_rq [Hactive_entity [Hstate [Hthread Hgeneration]]]]]]].
  assert (Hsame_rq : current_rq = rq).
  {
    destruct Hcurrent_rq as [_ Hcurrent_lookup].
    destruct Hactive_rq as [_ Htarget_lookup].
    rewrite Hcurrent_lookup in Htarget_lookup.
    inversion Htarget_lookup.
    reflexivity.
  }
  subst current_rq.
  destruct Hactive_entity as [Hlt Hlookup].
  unfold kernel_current_entity_index.
  apply kernel_current_entity_index_from_finds_running
    with
      (offset := current_index)
      (entity := current_entity);
    auto.
  - rewrite Hthread.
    exact Hthread_current.
  - rewrite Hgeneration.
    exact Hgeneration_current.
Qed.

Lemma compact_entities_keeps_in :
  forall kept entity,
    (length kept <= eevdf_max_entities)%nat ->
    In entity kept ->
    exists index,
      (index < length kept)%nat /\
      nth_error (compact_entities kept) index = Some entity.
Proof.
  intros kept entity Hlen Hin.
  apply In_nth_error in Hin as [index Hlookup].
  exists index.
  assert (Hindex_lt : (index < length kept)%nat).
  {
    apply nth_error_Some.
    rewrite Hlookup.
    discriminate.
  }
  split; [exact Hindex_lt |].
  unfold compact_entities.
  rewrite nth_error_firstn.
  replace (index <? eevdf_max_entities)%nat with true by
    (symmetry; apply Nat.ltb_lt; lia).
  rewrite nth_error_app1 by lia.
  exact Hlookup.
Qed.

Lemma compact_entities_length :
  forall kept,
    (length kept <= eevdf_max_entities)%nat ->
    length (compact_entities kept) = eevdf_max_entities.
Proof.
  intros kept Hlen.
  unfold compact_entities.
  rewrite length_firstn.
  rewrite length_app.
  rewrite repeat_length.
  lia.
Qed.

Lemma compact_entities_padding_empty :
  forall kept index entity,
    (length kept <= eevdf_max_entities)%nat ->
    nth_error (compact_entities kept) index = Some entity ->
    (length kept <= index)%nat ->
    entity = eevdf_empty_entity.
Proof.
  intros kept index entity Hlen Hlookup Hindex.
  unfold compact_entities in Hlookup.
  rewrite nth_error_firstn in Hlookup.
  destruct (index <? eevdf_max_entities)%nat eqn:Hfirstn;
    try discriminate.
  rewrite nth_error_app2 in Hlookup by lia.
  apply nth_error_repeat_some in Hlookup.
  exact Hlookup.
Qed.

Lemma compact_entities_lookup_kept :
  forall kept index entity,
    (length kept <= eevdf_max_entities)%nat ->
    (index < length kept)%nat ->
    nth_error (compact_entities kept) index = Some entity ->
    nth_error kept index = Some entity.
Proof.
  intros kept index entity Hlen Hindex Hlookup.
  unfold compact_entities in Hlookup.
  rewrite nth_error_firstn in Hlookup.
  replace (index <? eevdf_max_entities)%nat with true in Hlookup by
    (symmetry; apply Nat.ltb_lt; lia).
  rewrite nth_error_app1 in Hlookup by lia.
  exact Hlookup.
Qed.

Lemma eevdf_active_thread_ids_unique_active_list :
  forall rq,
    eevdf_active_thread_ids_unique rq ->
    list_active_thread_ids_unique
      (firstn (er_entity_count rq) (er_entities rq)).
Proof.
  intros rq Hunique.
  unfold list_active_thread_ids_unique.
  intros i j lhs rhs Hlhs Hrhs Hlhs_nonzero Hthreads_eq
    Hlhs_active Hrhs_active.
  rewrite nth_error_firstn in Hlhs.
  rewrite nth_error_firstn in Hrhs.
  destruct (i <? er_entity_count rq)%nat eqn:Hi; try discriminate.
  destruct (j <? er_entity_count rq)%nat eqn:Hj; try discriminate.
  apply Nat.ltb_lt in Hi.
  apply Nat.ltb_lt in Hj.
  eapply Hunique.
  - unfold eevdf_active_entity.
    split; eauto.
  - unfold eevdf_active_entity.
    split; eauto.
  - exact Hlhs_nonzero.
  - exact Hthreads_eq.
  - exact Hlhs_active.
  - exact Hrhs_active.
Qed.

Lemma remove_entity_kept_length_le :
  forall rq removed_thread,
    eevdf_shape rq ->
    (length
      (filter
        (fun entity => negb (Z.eqb (ee_thread_id entity) removed_thread))
        (firstn (er_entity_count rq) (er_entities rq))) <=
      eevdf_max_entities)%nat.
Proof.
  intros rq removed_thread Hshape.
  pose proof
    (filter_length_le
      (fun entity => negb (Z.eqb (ee_thread_id entity) removed_thread))
      (firstn (er_entity_count rq) (er_entities rq))) as Hfilter.
  destruct Hshape as [Hcount _Hlen].
  rewrite length_firstn in Hfilter.
  lia.
Qed.

Lemma remove_entity_from_runqueue_shape :
  forall rq removed_thread,
    eevdf_shape rq ->
    eevdf_shape (remove_entity_from_runqueue rq removed_thread).
Proof.
  intros rq removed_thread Hshape.
  unfold remove_entity_from_runqueue.
  apply refresh_runqueue_shape.
  unfold eevdf_shape.
  simpl.
  set (kept :=
    filter
      (fun entity => negb (Z.eqb (ee_thread_id entity) removed_thread))
      (firstn (er_entity_count rq) (er_entities rq))).
  assert (Hkept_len : (length kept <= eevdf_max_entities)%nat).
  {
    unfold kept.
    apply remove_entity_kept_length_le.
    exact Hshape.
  }
  split.
  - exact Hkept_len.
  - apply compact_entities_length.
    exact Hkept_len.
Qed.

Lemma remove_entity_from_runqueue_inactive_entities_empty :
  forall rq removed_thread,
    eevdf_shape rq ->
    eevdf_inactive_entities_empty
      (remove_entity_from_runqueue rq removed_thread).
Proof.
  intros rq removed_thread Hshape.
  unfold remove_entity_from_runqueue.
  apply refresh_runqueue_inactive_entities_empty.
  unfold eevdf_inactive_entities_empty.
  intros index entity Hlookup Hindex.
  simpl in Hlookup.
  simpl in Hindex.
  set (kept :=
    filter
      (fun entity => negb (Z.eqb (ee_thread_id entity) removed_thread))
      (firstn (er_entity_count rq) (er_entities rq))) in *.
  assert (Hkept_len : (length kept <= eevdf_max_entities)%nat).
  {
    unfold kept.
    apply remove_entity_kept_length_le.
    exact Hshape.
  }
  pose proof
    (compact_entities_padding_empty kept index entity Hkept_len Hlookup Hindex)
    as Hempty.
  subst entity.
  reflexivity.
Qed.

Lemma remove_entity_from_runqueue_active_thread_ids_unique :
  forall rq removed_thread,
    eevdf_shape rq ->
    eevdf_active_thread_ids_unique rq ->
    eevdf_active_thread_ids_unique
      (remove_entity_from_runqueue rq removed_thread).
Proof.
  intros rq removed_thread Hshape Hunique.
  unfold remove_entity_from_runqueue.
  apply refresh_runqueue_active_thread_ids_unique.
  set (active := firstn (er_entity_count rq) (er_entities rq)).
  set (kept :=
    filter
      (fun entity => negb (Z.eqb (ee_thread_id entity) removed_thread))
      active).
  assert (Hkept_len : (length kept <= eevdf_max_entities)%nat).
  {
    unfold kept, active.
    apply remove_entity_kept_length_le.
    exact Hshape.
  }
  assert (Hkept_unique : list_active_thread_ids_unique kept).
  {
    unfold kept, active.
    apply list_active_thread_ids_unique_filter.
    apply eevdf_active_thread_ids_unique_active_list.
    exact Hunique.
  }
  unfold eevdf_active_thread_ids_unique.
  intros i j lhs rhs Hlhs Hrhs Hlhs_nonzero Hthreads_eq
    Hlhs_active Hrhs_active.
  destruct Hlhs as [Hi_lt Hlhs_lookup].
  destruct Hrhs as [Hj_lt Hrhs_lookup].
  simpl in Hi_lt.
  simpl in Hj_lt.
  simpl in Hlhs_lookup.
  simpl in Hrhs_lookup.
  pose proof
    (compact_entities_lookup_kept kept i lhs Hkept_len Hi_lt Hlhs_lookup)
    as Hlhs_kept.
  pose proof
    (compact_entities_lookup_kept kept j rhs Hkept_len Hj_lt Hrhs_lookup)
    as Hrhs_kept.
  unfold list_active_thread_ids_unique in Hkept_unique.
  eapply Hkept_unique; eauto.
Qed.

Lemma remove_entity_from_runqueue_keeps_other_active :
  forall rq removed_thread index entity,
    eevdf_shape rq ->
    eevdf_active_entity rq index entity ->
    ee_thread_id entity <> removed_thread ->
    exists new_index,
      eevdf_active_entity
        (remove_entity_from_runqueue rq removed_thread)
        new_index
        entity.
Proof.
  intros rq removed_thread index entity Hshape Hactive Hthread_neq.
  destruct Hactive as [Hlt Hlookup].
  unfold remove_entity_from_runqueue.
  set (active := firstn (er_entity_count rq) (er_entities rq)).
  set (kept :=
    filter
      (fun entity => negb (Z.eqb (ee_thread_id entity) removed_thread))
      active).
  assert (Hactive_lookup : nth_error active index = Some entity).
  {
    unfold active.
    rewrite nth_error_firstn.
    replace (index <? er_entity_count rq)%nat with true by
      (symmetry; apply Nat.ltb_lt; exact Hlt).
    exact Hlookup.
  }
  assert (Hin_kept : In entity kept).
  {
    unfold kept.
    apply filter_In.
    split.
    - apply nth_error_In in Hactive_lookup.
      exact Hactive_lookup.
    - apply negb_true_iff.
      apply Z.eqb_neq.
      exact Hthread_neq.
  }
  assert (Hkept_len : (length kept <= eevdf_max_entities)%nat).
  {
    unfold kept, active.
    pose proof
      (filter_length_le
        (fun entity => negb (Z.eqb (ee_thread_id entity) removed_thread))
        (firstn (er_entity_count rq) (er_entities rq))) as Hfilter.
    destruct Hshape as [Hcount _Hentities_len].
    rewrite length_firstn in Hfilter.
    lia.
  }
  destruct (compact_entities_keeps_in kept entity Hkept_len Hin_kept)
    as [new_index [Hnew_lt Hnew_lookup]].
  exists new_index.
  apply (proj2 (refresh_active_entity_iff _ _ _)).
  unfold eevdf_active_entity.
  simpl.
  split.
  - exact Hnew_lt.
  - exact Hnew_lookup.
Qed.

Lemma remove_entity_from_runqueue_active_from_original :
  forall rq removed_thread index entity,
    eevdf_shape rq ->
    eevdf_active_entity
      (remove_entity_from_runqueue rq removed_thread)
      index
      entity ->
    exists original_index,
      eevdf_active_entity rq original_index entity /\
      ee_thread_id entity <> removed_thread.
Proof.
  intros rq removed_thread index entity Hshape Hactive_removed.
  unfold remove_entity_from_runqueue in Hactive_removed.
  set (active := firstn (er_entity_count rq) (er_entities rq)) in *.
  set (kept :=
    filter
      (fun entity => negb (Z.eqb (ee_thread_id entity) removed_thread))
      active) in *.
  apply (proj1 (refresh_active_entity_iff _ _ _)) in Hactive_removed.
  destruct Hactive_removed as [Hindex_lt Hlookup_compact].
  simpl in Hindex_lt.
  simpl in Hlookup_compact.
  assert (Hlookup_kept : nth_error kept index = Some entity).
  {
    unfold compact_entities in Hlookup_compact.
    rewrite nth_error_firstn in Hlookup_compact.
    replace (index <? eevdf_max_entities)%nat with true in Hlookup_compact.
    - rewrite nth_error_app1 in Hlookup_compact by lia.
      exact Hlookup_compact.
    - symmetry.
      apply Nat.ltb_lt.
      assert (Hkept_len : (length kept <= eevdf_max_entities)%nat).
      {
        unfold kept, active.
        pose proof
          (filter_length_le
            (fun entity => negb (Z.eqb (ee_thread_id entity) removed_thread))
            (firstn (er_entity_count rq) (er_entities rq))) as Hfilter.
        destruct Hshape as [Hcount _Hentities_len].
        rewrite length_firstn in Hfilter.
        lia.
      }
      lia.
  }
  apply nth_error_In in Hlookup_kept as Hin_kept.
  unfold kept in Hin_kept.
  apply filter_In in Hin_kept as [Hin_active Hkeep].
  apply In_nth_error in Hin_active as [original_index Hactive_lookup].
  exists original_index.
  split.
  - unfold eevdf_active_entity.
    unfold active in Hactive_lookup.
    rewrite nth_error_firstn in Hactive_lookup.
    destruct (original_index <? er_entity_count rq)%nat eqn:Hlt;
      try discriminate.
    apply Nat.ltb_lt in Hlt.
    split; auto.
  - apply negb_true_iff in Hkeep.
    apply Z.eqb_neq in Hkeep.
    exact Hkeep.
Qed.

Lemma remove_entity_from_runqueue_positive_entity_params :
  forall rq removed_thread,
    eevdf_shape rq ->
    eevdf_positive_entity_params rq ->
    eevdf_positive_entity_params
      (remove_entity_from_runqueue rq removed_thread).
Proof.
  intros rq removed_thread Hshape Hpositive.
  unfold eevdf_positive_entity_params.
  intros index entity Hactive Hactive_state.
  destruct (remove_entity_from_runqueue_active_from_original
    rq
    removed_thread
    index
    entity
    Hshape
    Hactive) as [original_index [Horiginal_active _Hkept]].
  eapply Hpositive; eauto.
Qed.

Lemma remove_entity_from_runqueue_runnable_count_consistent :
  forall rq removed_thread,
    eevdf_runnable_count_consistent
      (remove_entity_from_runqueue rq removed_thread).
Proof.
  intros rq removed_thread.
  unfold remove_entity_from_runqueue.
  apply refresh_runqueue_runnable_count_consistent.
Qed.

Lemma remove_entity_from_runqueue_live_deadlines_consistent :
  forall rq removed_thread,
    eevdf_invariant rq ->
    eevdf_live_deadlines_consistent
      (remove_entity_from_runqueue rq removed_thread).
Proof.
  intros rq removed_thread Hinv.
  pose proof Hinv as Hinv_full.
  destruct Hinv as
    [Hshape
    [_Hinactive
    [_Hunique
    [_Hpositive
    [_Hlive
    [_Hmin
    [_Hvirtual _Hrunnable]]]]]]].
  unfold eevdf_live_deadlines_consistent.
  intros index entity Hactive Hlive_entity.
  destruct (remove_entity_from_runqueue_active_from_original
    rq
    removed_thread
    index
    entity
    Hshape
    Hactive) as [original_index [Horiginal_active _Hkept]].
  destruct (invariant_live_deadline_at_vruntime
    rq
    original_index
    entity
    Hinv_full
    Horiginal_active
    Hlive_entity) as [slice [Hslice [Heligible Hdeadline]]].
  exists slice.
  split; [exact Hslice |].
  split.
  - rewrite Heligible.
    symmetry.
    apply z_max_left_when_ge.
    eapply refresh_runqueue_min_vruntime_lower_bound.
    + exact Hactive.
    + exact Hlive_entity.
  - split.
    + rewrite Heligible.
      eapply refresh_runqueue_min_vruntime_lower_bound.
      * exact Hactive.
      * exact Hlive_entity.
    + rewrite Hdeadline.
      rewrite Heligible.
      reflexivity.
Qed.

Lemma remove_entity_from_runqueue_min_vruntime_consistent :
  forall rq removed_thread,
    eevdf_min_vruntime_consistent
      (remove_entity_from_runqueue rq removed_thread).
Proof.
  intros rq removed_thread.
  unfold remove_entity_from_runqueue.
  apply refresh_runqueue_min_vruntime_consistent.
Qed.

Lemma remove_entity_from_runqueue_virtual_time_consistent :
  forall rq removed_thread,
    eevdf_virtual_time_consistent
      (remove_entity_from_runqueue rq removed_thread).
Proof.
  intros rq removed_thread.
  unfold remove_entity_from_runqueue.
  apply refresh_runqueue_virtual_time_consistent.
Qed.

Theorem remove_entity_from_runqueue_preserves_basic_safety :
  forall rq removed_thread,
    eevdf_invariant rq ->
    eevdf_invariant_without_thread_uniqueness
      (remove_entity_from_runqueue rq removed_thread).
Proof.
  intros rq removed_thread Hinv.
  pose proof Hinv as Hinv_full.
  destruct Hinv as
    [Hshape
    [_Hinactive
    [_Hunique
    [Hpositive
    [_Hlive
    [_Hmin
    [_Hvirtual _Hrunnable]]]]]]].
  split.
  - apply remove_entity_from_runqueue_shape.
    exact Hshape.
  - split.
    + apply remove_entity_from_runqueue_inactive_entities_empty.
      exact Hshape.
    + split.
      * apply remove_entity_from_runqueue_positive_entity_params; auto.
      * split.
        -- apply remove_entity_from_runqueue_live_deadlines_consistent.
           exact Hinv_full.
        -- split.
           ++ apply remove_entity_from_runqueue_min_vruntime_consistent.
           ++ split.
              ** apply remove_entity_from_runqueue_virtual_time_consistent.
              ** apply remove_entity_from_runqueue_runnable_count_consistent.
Qed.

Theorem remove_entity_from_runqueue_preserves_invariant :
  forall rq removed_thread,
    eevdf_invariant rq ->
    eevdf_invariant (remove_entity_from_runqueue rq removed_thread).
Proof.
  intros rq removed_thread Hinv.
  pose proof Hinv as Hinv_full.
  destruct Hinv as
    [Hshape
    [_Hinactive
    [Hunique
    [_Hpositive
    [_Hlive
    [_Hmin
    [_Hvirtual _Hrunnable]]]]]]].
  apply eevdf_invariant_intro.
  - apply remove_entity_from_runqueue_shape.
    exact Hshape.
  - apply remove_entity_from_runqueue_inactive_entities_empty.
    exact Hshape.
  - apply remove_entity_from_runqueue_active_thread_ids_unique; auto.
  - destruct (remove_entity_from_runqueue_preserves_basic_safety
      rq removed_thread Hinv_full) as [_ [_ [Hpositive _]]].
    exact Hpositive.
  - apply remove_entity_from_runqueue_live_deadlines_consistent.
    exact Hinv_full.
  - apply remove_entity_from_runqueue_min_vruntime_consistent.
  - apply remove_entity_from_runqueue_virtual_time_consistent.
  - apply remove_entity_from_runqueue_runnable_count_consistent.
Qed.

Lemma refresh_append_entity_keeps_existing_active :
  forall rq appended index entity,
    eevdf_shape rq ->
    eevdf_active_entity rq index entity ->
    eevdf_active_entity
      (refresh_runqueue (append_entity rq appended))
      index
      entity.
Proof.
  intros rq appended index entity Hshape Hactive.
  apply (proj2 (refresh_active_entity_iff _ _ _)).
  destruct Hactive as [Hlt Hlookup].
  unfold eevdf_active_entity, append_entity.
  simpl.
  split.
  - lia.
  - rewrite nth_error_replace_nth_neq by lia.
    exact Hlookup.
Qed.

Lemma kernel_migrating_runnable_differs_from_running_entity :
  forall sched src_cpu src migrate_index migrated
    run_cpu run_rq run_index running thread_id,
    kernel_sched_invariant sched ->
    kernel_active_runqueue sched src_cpu src ->
    find_entity_index src thread_id = Some migrate_index ->
    lookup_entity src migrate_index = Some migrated ->
    ee_state migrated = ERunnable ->
    kernel_active_runqueue sched run_cpu run_rq ->
    eevdf_active_entity run_rq run_index running ->
    ee_state running = ERunning ->
    ee_thread_id running <> thread_id.
Proof.
  intros sched src_cpu src migrate_index migrated
    run_cpu run_rq run_index running thread_id
    Hinv Hsrc_active Hfind Hlookup Hmigrated_state
    Hrun_active Hrunning_active Hrunning_state Heq.
  pose proof
    (find_entity_index_lookup_matches_thread
      src
      thread_id
      migrate_index
      migrated
      Hfind
      Hlookup) as [Hmigrated_thread Hmigrated_active_state].
  pose proof
    (find_entity_index_thread_not_none
      src
      thread_id
      migrate_index
      Hfind) as Hthread_not_none.
  destruct Hinv as
    [_Hshape
      [_Hrqs
      [Hunique
      [_Hcurrent
      [_Hrunning
      [_Hnodup _Htargets]]]]]].
  assert (Hmigrated_on :
    kernel_entity_on_cpu sched src_cpu migrate_index migrated).
  {
    unfold kernel_entity_on_cpu.
    exists src.
    split.
    - exact Hsrc_active.
    - split.
      + apply find_entity_index_active with (thread_id := thread_id).
        * exact Hfind.
        * exact Hlookup.
      + rewrite Hmigrated_state.
        reflexivity.
  }
  assert (Hrunning_on :
    kernel_entity_on_cpu sched run_cpu run_index running).
  {
    unfold kernel_entity_on_cpu.
    exists run_rq.
    split.
    - exact Hrun_active.
    - split.
      + exact Hrunning_active.
      + rewrite Hrunning_state.
        reflexivity.
  }
  assert (Hsame :
    src_cpu = run_cpu /\ migrate_index = run_index).
  {
    eapply Hunique.
    - exact Hmigrated_on.
    - exact Hrunning_on.
    - rewrite Hmigrated_thread.
      exact Hthread_not_none.
    - rewrite Hmigrated_thread.
      symmetry.
      exact Heq.
  }
  destruct Hsame as [Hsame_cpu Hsame_index].
  subst run_cpu run_index.
  destruct Hsrc_active as [_ Hsrc_lookup].
  destruct Hrun_active as [_ Hrun_lookup].
  rewrite Hsrc_lookup in Hrun_lookup.
  inversion Hrun_lookup; subst run_rq.
  destruct Hrunning_active as [_ Hrunning_lookup].
  unfold lookup_entity in Hlookup.
  rewrite Hlookup in Hrunning_lookup.
  inversion Hrunning_lookup; subst running.
  rewrite Hmigrated_state in Hrunning_state.
  discriminate.
Qed.

Lemma entity_as_migrated_runnable_state :
  forall dst entity moved,
    entity_as_migrated_runnable dst entity = Some moved ->
    ee_state moved = ERunnable.
Proof.
  intros dst entity moved Hmoved.
  unfold entity_as_migrated_runnable in Hmoved.
  unfold refresh_deadline in Hmoved.
  destruct (weighted_slice (ee_slice_ns _) (ee_weight _)) as [slice |];
    try discriminate.
  destruct (i64_value _) eqn:Hdeadline; try discriminate.
  inversion Hmoved.
  reflexivity.
Qed.

Lemma entity_as_migrated_runnable_properties :
  forall dst entity moved,
    entity_as_migrated_runnable dst entity = Some moved ->
    ee_thread_id moved = ee_thread_id entity /\
    ee_generation moved = ee_generation entity /\
    ee_weight moved = ee_weight entity /\
    ee_slice_ns moved = ee_slice_ns entity /\
    ee_service_ns moved = ee_service_ns entity /\
    er_min_vruntime dst <= ee_vruntime moved /\
    (live_for_min moved = true ->
      er_min_vruntime dst <= ee_vruntime moved /\
      exists slice,
        weighted_slice (ee_slice_ns moved) (ee_weight moved) =
          Some slice /\
        ee_eligible_time moved = ee_vruntime moved /\
        ee_deadline moved = ee_vruntime moved + slice).
Proof.
  intros dst entity moved Hmoved.
  unfold entity_as_migrated_runnable in Hmoved.
  pose
    (seed := {|
      ee_thread_id := ee_thread_id entity;
      ee_generation := ee_generation entity;
      ee_weight := ee_weight entity;
      ee_slice_ns := ee_slice_ns entity;
      ee_service_ns := ee_service_ns entity;
      ee_vruntime := z_max (ee_vruntime entity) (er_min_vruntime dst);
      ee_eligible_time := ee_eligible_time entity;
      ee_deadline := ee_deadline entity;
      ee_state := ERunnable;
    |}).
  assert (Hrefresh : refresh_deadline seed (er_min_vruntime dst) = Some moved)
    by exact Hmoved.
  pose proof (refresh_deadline_success seed (er_min_vruntime dst) moved Hrefresh)
    as Hsuccess.
  destruct Hsuccess as
    [Hthread
    [Hgeneration
    [Hweight
    [Hslice_ns
    [Hservice
    [Hvruntime
    [_Hstate Hdeadline]]]]]]].
  assert (Hfloor : er_min_vruntime dst <= ee_vruntime moved).
  {
    rewrite Hvruntime.
    simpl.
    apply z_max_ge_r.
  }
  split; [exact Hthread |].
  split; [exact Hgeneration |].
  split; [exact Hweight |].
  split; [exact Hslice_ns |].
  split; [exact Hservice |].
  split; [exact Hfloor |].
  intros _Hlive.
  split; [exact Hfloor |].
  destruct Hdeadline as [slice [Hslice [Heligible [_Heligible_floor Hdeadline]]]].
  exists slice.
  repeat split.
  - rewrite Hslice_ns.
    rewrite Hweight.
    exact Hslice.
  - rewrite Heligible.
    rewrite Hvruntime.
    simpl.
    rewrite z_max_left_when_ge.
    + reflexivity.
    + apply z_max_ge_r.
  - rewrite Hdeadline.
    rewrite Heligible.
    rewrite Hvruntime.
    simpl.
    rewrite z_max_left_when_ge.
    + reflexivity.
    + apply z_max_ge_r.
Qed.

Lemma kernel_replace_cpu_shape :
  forall sched cpu_idx cpu,
    kernel_sched_shape sched ->
    kernel_sched_shape (kernel_replace_cpu sched cpu_idx cpu).
Proof.
  intros sched cpu_idx cpu [Hcount [Hrqs_len Hcpus_len]].
  unfold kernel_sched_shape, kernel_replace_cpu.
  simpl.
  repeat split; auto.
  rewrite replace_nth_length.
  exact Hcpus_len.
Qed.

Lemma kernel_replace_runqueue_shape :
  forall sched cpu_idx rq,
    kernel_sched_shape sched ->
    kernel_sched_shape (kernel_replace_runqueue sched cpu_idx rq).
Proof.
  intros sched cpu_idx rq [Hcount [Hrqs_len Hcpus_len]].
  unfold kernel_sched_shape, kernel_replace_runqueue.
  simpl.
  repeat split; auto.
  rewrite replace_nth_length.
  exact Hrqs_len.
Qed.

Lemma kernel_set_activation_pending_shape :
  forall sched cpu_idx pending,
    kernel_sched_shape sched ->
    kernel_sched_shape (kernel_set_activation_pending sched cpu_idx pending).
Proof.
  intros sched cpu_idx pending Hshape.
  unfold kernel_set_activation_pending.
  destruct (kernel_lookup_cpu sched cpu_idx) as [cpu |].
  - apply kernel_replace_cpu_shape.
    exact Hshape.
  - exact Hshape.
Qed.

Lemma kernel_set_cpu_current_shape :
  forall sched cpu_idx thread_id generation,
    kernel_sched_shape sched ->
    kernel_sched_shape
      (kernel_set_cpu_current sched cpu_idx thread_id generation).
Proof.
  intros sched cpu_idx thread_id generation Hshape.
  unfold kernel_set_cpu_current.
  destruct (kernel_lookup_cpu sched cpu_idx) as [cpu |].
  - apply kernel_replace_cpu_shape.
    exact Hshape.
  - exact Hshape.
Qed.

Lemma kernel_clear_cpu_current_shape :
  forall sched cpu_idx,
    kernel_sched_shape sched ->
    kernel_sched_shape (kernel_clear_cpu_current sched cpu_idx).
Proof.
  intros sched cpu_idx Hshape.
  unfold kernel_clear_cpu_current.
  destruct (kernel_lookup_cpu sched cpu_idx) as [cpu |].
  - apply kernel_replace_cpu_shape.
    exact Hshape.
  - exact Hshape.
Qed.

Lemma kernel_replace_runqueue_same :
  forall sched cpu_idx rq,
    kernel_lookup_runqueue sched cpu_idx = Some rq ->
    kernel_replace_runqueue sched cpu_idx rq = sched.
Proof.
  intros sched cpu_idx rq Hlookup.
  unfold kernel_lookup_runqueue in Hlookup.
  unfold kernel_replace_runqueue.
  destruct sched as [runqueues cpus cpu_count balance_cursor].
  simpl in *.
  rewrite replace_nth_same with (item := rq); auto.
Qed.

Lemma kernel_set_cpu_current_preserves_runqueues :
  forall sched cpu_idx thread_id generation,
    ks_runqueues
      (kernel_set_cpu_current sched cpu_idx thread_id generation) =
    ks_runqueues sched.
Proof.
  intros sched cpu_idx thread_id generation.
  unfold kernel_set_cpu_current.
  destruct (kernel_lookup_cpu sched cpu_idx); reflexivity.
Qed.

Lemma kernel_clear_cpu_current_preserves_runqueues :
  forall sched cpu_idx,
    ks_runqueues (kernel_clear_cpu_current sched cpu_idx) =
    ks_runqueues sched.
Proof.
  intros sched cpu_idx.
  unfold kernel_clear_cpu_current.
  destruct (kernel_lookup_cpu sched cpu_idx); reflexivity.
Qed.

Lemma kernel_set_activation_pending_preserves_runqueues :
  forall sched cpu_idx pending,
    ks_runqueues
      (kernel_set_activation_pending sched cpu_idx pending) =
    ks_runqueues sched.
Proof.
  intros sched cpu_idx pending.
  unfold kernel_set_activation_pending.
  destruct (kernel_lookup_cpu sched cpu_idx); reflexivity.
Qed.

Lemma kernel_replace_runqueue_preserves_cpus :
  forall sched cpu_idx rq,
    ks_cpus (kernel_replace_runqueue sched cpu_idx rq) =
    ks_cpus sched.
Proof.
  reflexivity.
Qed.

Lemma kernel_set_activation_pending_lookup_cpu_eq :
  forall sched cpu_idx old_cpu pending,
    kernel_sched_shape sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_lookup_cpu sched cpu_idx = Some old_cpu ->
    kernel_lookup_cpu
      (kernel_set_activation_pending sched cpu_idx pending)
      cpu_idx =
      Some
        {|
          kc_has_current := kc_has_current old_cpu;
          kc_current_thread_id := kc_current_thread_id old_cpu;
          kc_current_generation := kc_current_generation old_cpu;
          kc_activation_pending := pending;
        |}.
Proof.
  intros sched cpu_idx old_cpu pending [Hcount [_ Hcpus_len]] Hvalid Hlookup.
  unfold kernel_set_activation_pending.
  rewrite Hlookup.
  unfold kernel_lookup_cpu, kernel_replace_cpu.
  simpl.
  apply nth_error_replace_nth_eq.
  rewrite Hcpus_len.
  pose proof (kernel_valid_cpu_lt sched cpu_idx Hvalid) as Hlt.
  lia.
Qed.

Lemma kernel_replace_runqueue_preserves_cpu_count :
  forall sched cpu_idx rq,
    ks_cpu_count (kernel_replace_runqueue sched cpu_idx rq) =
    ks_cpu_count sched.
Proof.
  reflexivity.
Qed.

Lemma kernel_set_cpu_current_preserves_cpu_count :
  forall sched cpu_idx thread_id generation,
    ks_cpu_count
      (kernel_set_cpu_current sched cpu_idx thread_id generation) =
    ks_cpu_count sched.
Proof.
  intros sched cpu_idx thread_id generation.
  unfold kernel_set_cpu_current.
  destruct (kernel_lookup_cpu sched cpu_idx); reflexivity.
Qed.

Lemma kernel_clear_cpu_current_preserves_cpu_count :
  forall sched cpu_idx,
    ks_cpu_count (kernel_clear_cpu_current sched cpu_idx) =
    ks_cpu_count sched.
Proof.
  intros sched cpu_idx.
  unfold kernel_clear_cpu_current.
  destruct (kernel_lookup_cpu sched cpu_idx); reflexivity.
Qed.

Lemma kernel_set_activation_pending_preserves_cpu_count :
  forall sched cpu_idx pending,
    ks_cpu_count
      (kernel_set_activation_pending sched cpu_idx pending) =
    ks_cpu_count sched.
Proof.
  intros sched cpu_idx pending.
  unfold kernel_set_activation_pending.
  destruct (kernel_lookup_cpu sched cpu_idx); reflexivity.
Qed.

Lemma kernel_set_cpu_current_sets_cpu_slot :
  forall sched cpu_idx old_cpu thread_id generation,
    kernel_sched_shape sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_lookup_cpu sched cpu_idx = Some old_cpu ->
    kernel_cpu_slot
      (kernel_set_cpu_current sched cpu_idx thread_id generation)
      cpu_idx
      {|
        kc_has_current := true;
        kc_current_thread_id := thread_id;
        kc_current_generation := generation;
        kc_activation_pending := kc_activation_pending old_cpu;
      |}.
Proof.
  intros sched cpu_idx old_cpu thread_id generation
    [Hcount [_ Hcpus_len]] Hvalid Hlookup.
  unfold kernel_set_cpu_current.
  rewrite Hlookup.
  unfold kernel_cpu_slot, kernel_replace_cpu.
  simpl.
  split.
  - apply kernel_valid_cpu_lt.
    exact Hvalid.
  - apply nth_error_replace_nth_eq.
    rewrite Hcpus_len.
    pose proof (kernel_valid_cpu_lt sched cpu_idx Hvalid) as Hlt.
    lia.
Qed.

Lemma kernel_replace_runqueue_preserves_cpu_slot :
  forall sched target_cpu rq query_cpu cpu,
    kernel_cpu_slot
      (kernel_replace_runqueue sched target_cpu rq)
      query_cpu
      cpu ->
    kernel_cpu_slot sched query_cpu cpu.
Proof.
  intros sched target_cpu rq query_cpu cpu [Hlt Hlookup].
  unfold kernel_cpu_slot, kernel_replace_runqueue in *.
  simpl in *.
  split; auto.
Qed.

Lemma kernel_replace_runqueue_preserves_cpu_slot_forward :
  forall sched target_cpu rq query_cpu cpu,
    kernel_cpu_slot sched query_cpu cpu ->
    kernel_cpu_slot
      (kernel_replace_runqueue sched target_cpu rq)
      query_cpu
      cpu.
Proof.
  intros sched target_cpu rq query_cpu cpu [Hlt Hlookup].
  unfold kernel_cpu_slot, kernel_replace_runqueue in *.
  simpl in *.
  split; auto.
Qed.

Lemma kernel_set_activation_pending_preserves_other_cpu_slot :
  forall sched target_cpu query_cpu pending cpu,
    query_cpu <> target_cpu ->
    kernel_cpu_slot
      (kernel_set_activation_pending sched target_cpu pending)
      query_cpu
      cpu ->
    kernel_cpu_slot sched query_cpu cpu.
Proof.
  intros sched target_cpu query_cpu pending cpu Hneq Hslot.
  unfold kernel_set_activation_pending in Hslot.
  destruct (kernel_lookup_cpu sched target_cpu) as [target |] eqn:Hlookup.
  - destruct Hslot as [Hlt Hslot_lookup].
    unfold kernel_replace_cpu in Hslot_lookup.
    simpl in Hslot_lookup.
    rewrite nth_error_replace_nth_neq in Hslot_lookup by congruence.
    unfold kernel_cpu_slot.
    simpl in Hlt.
    split; auto.
  - exact Hslot.
Qed.

Lemma kernel_set_activation_pending_preserves_other_cpu_slot_forward :
  forall sched target_cpu query_cpu pending cpu,
    query_cpu <> target_cpu ->
    kernel_cpu_slot sched query_cpu cpu ->
    kernel_cpu_slot
      (kernel_set_activation_pending sched target_cpu pending)
      query_cpu
      cpu.
Proof.
  intros sched target_cpu query_cpu pending cpu Hneq Hslot.
  unfold kernel_set_activation_pending.
  destruct (kernel_lookup_cpu sched target_cpu) as [target |] eqn:Hlookup.
  - destruct Hslot as [Hlt Hslot_lookup].
    unfold kernel_cpu_slot, kernel_replace_cpu.
    simpl.
    split; auto.
    rewrite nth_error_replace_nth_neq by congruence.
    exact Hslot_lookup.
  - exact Hslot.
Qed.

Lemma kernel_set_cpu_current_preserves_other_cpu_slot :
  forall sched target_cpu query_cpu thread_id generation cpu,
    query_cpu <> target_cpu ->
    kernel_cpu_slot
      (kernel_set_cpu_current sched target_cpu thread_id generation)
      query_cpu
      cpu ->
    kernel_cpu_slot sched query_cpu cpu.
Proof.
  intros sched target_cpu query_cpu thread_id generation cpu Hneq Hslot.
  unfold kernel_set_cpu_current in Hslot.
  destruct (kernel_lookup_cpu sched target_cpu) as [target |] eqn:Hlookup.
  - destruct Hslot as [Hlt Hslot_lookup].
    unfold kernel_replace_cpu in Hslot_lookup.
    simpl in Hslot_lookup.
    rewrite nth_error_replace_nth_neq in Hslot_lookup by congruence.
    unfold kernel_cpu_slot.
    simpl in Hlt.
    split; auto.
  - exact Hslot.
Qed.

Lemma kernel_clear_cpu_current_preserves_other_cpu_slot :
  forall sched target_cpu query_cpu cpu,
    query_cpu <> target_cpu ->
    kernel_cpu_slot
      (kernel_clear_cpu_current sched target_cpu)
      query_cpu
      cpu ->
    kernel_cpu_slot sched query_cpu cpu.
Proof.
  intros sched target_cpu query_cpu cpu Hneq Hslot.
  unfold kernel_clear_cpu_current in Hslot.
  destruct (kernel_lookup_cpu sched target_cpu) as [target |] eqn:Hlookup.
  - destruct Hslot as [Hlt Hslot_lookup].
    unfold kernel_replace_cpu in Hslot_lookup.
    simpl in Hslot_lookup.
    rewrite nth_error_replace_nth_neq in Hslot_lookup by congruence.
    unfold kernel_cpu_slot.
    simpl in Hlt.
    split; auto.
  - exact Hslot.
Qed.

Lemma kernel_clear_cpu_current_preserves_other_cpu_slot_forward :
  forall sched target_cpu query_cpu cpu,
    query_cpu <> target_cpu ->
    kernel_cpu_slot sched query_cpu cpu ->
    kernel_cpu_slot
      (kernel_clear_cpu_current sched target_cpu)
      query_cpu
      cpu.
Proof.
  intros sched target_cpu query_cpu cpu Hneq Hslot.
  unfold kernel_clear_cpu_current.
  destruct (kernel_lookup_cpu sched target_cpu) as [target |] eqn:Hlookup.
  - destruct Hslot as [Hlt Hslot_lookup].
    unfold kernel_cpu_slot, kernel_replace_cpu.
    simpl.
    split; auto.
    rewrite nth_error_replace_nth_neq by congruence.
    exact Hslot_lookup.
  - exact Hslot.
Qed.

Lemma kernel_set_cpu_current_preserves_other_cpu_slot_forward :
  forall sched target_cpu query_cpu thread_id generation cpu,
    query_cpu <> target_cpu ->
    kernel_cpu_slot sched query_cpu cpu ->
    kernel_cpu_slot
      (kernel_set_cpu_current sched target_cpu thread_id generation)
      query_cpu
      cpu.
Proof.
  intros sched target_cpu query_cpu thread_id generation cpu Hneq Hslot.
  unfold kernel_set_cpu_current.
  destruct (kernel_lookup_cpu sched target_cpu) as [target |] eqn:Hlookup.
  - destruct Hslot as [Hlt Hslot_lookup].
    unfold kernel_cpu_slot, kernel_replace_cpu.
    simpl.
    split; auto.
    rewrite nth_error_replace_nth_neq by congruence.
    exact Hslot_lookup.
  - exact Hslot.
Qed.

Lemma kernel_replace_cpu_preserves_runqueues_invariant :
  forall sched cpu_idx cpu,
    kernel_runqueues_invariant sched ->
    kernel_runqueues_invariant (kernel_replace_cpu sched cpu_idx cpu).
Proof.
  intros sched cpu_idx cpu Hrqs.
  unfold kernel_runqueues_invariant in *.
  intros rq_cpu local_rq [Hlt Hlookup].
  apply (Hrqs rq_cpu local_rq).
  unfold kernel_active_runqueue.
  simpl in *.
  split; auto.
Qed.

Lemma kernel_replace_cpu_preserves_global_thread_ids_unique :
  forall sched cpu_idx cpu,
    kernel_global_thread_ids_unique sched ->
    kernel_global_thread_ids_unique (kernel_replace_cpu sched cpu_idx cpu).
Proof.
  intros sched cpu_idx cpu Hunique.
  unfold kernel_global_thread_ids_unique in *.
  intros cpu_a index_a cpu_b index_b lhs rhs Hlhs Hrhs Hthread Heq.
  apply Hunique with
    (lhs := lhs)
    (rhs := rhs); auto.
Qed.

Lemma kernel_replace_cpu_preserves_current_matches :
  forall sched cpu_idx old_cpu new_cpu,
    kernel_sched_shape sched ->
    kernel_lookup_cpu sched cpu_idx = Some old_cpu ->
    kc_has_current new_cpu = kc_has_current old_cpu ->
    kc_current_thread_id new_cpu = kc_current_thread_id old_cpu ->
    kc_current_generation new_cpu = kc_current_generation old_cpu ->
    kernel_current_matches_local_running sched ->
    kernel_current_matches_local_running
      (kernel_replace_cpu sched cpu_idx new_cpu).
Proof.
  intros sched cpu_idx old_cpu new_cpu Hshape Hlookup
    Hhas Hthread Hgen Hcurrent.
  unfold kernel_current_matches_local_running in *.
  intros query_cpu cpu Hslot Hcpu_has_current.
  unfold kernel_cpu_slot, kernel_replace_cpu in Hslot.
  simpl in Hslot.
  destruct Hslot as [Hlt Hslot_lookup].
  destruct (Nat.eq_dec query_cpu cpu_idx) as [Heq | Hneq].
  - subst query_cpu.
    rewrite nth_error_replace_nth_eq in Hslot_lookup.
    + inversion Hslot_lookup; subst cpu.
      unfold kernel_lookup_cpu in Hlookup.
      destruct Hshape as [Hcount [_ Hcpus_len]].
      pose proof Hlookup as Hslot_old.
      destruct (Hcurrent cpu_idx old_cpu) as [rq [index [entity Hresult]]].
      * unfold kernel_cpu_slot.
        split.
        -- lia.
        -- exact Hslot_old.
      * rewrite <- Hhas.
        exact Hcpu_has_current.
      * exists rq, index, entity.
        destruct Hresult as [Hactive [Hentity [Hstate [Htid Hgeneration]]]].
        split.
        -- unfold kernel_active_runqueue, kernel_replace_cpu in *.
           simpl in *.
           exact Hactive.
        -- split.
           ++ exact Hentity.
           ++ repeat split.
              ** exact Hstate.
              ** rewrite Hthread.
                 exact Htid.
              ** rewrite Hgen.
                 exact Hgeneration.
    + destruct Hshape as [Hcount [_ Hcpus_len]].
      rewrite Hcpus_len.
      lia.
  - rewrite nth_error_replace_nth_neq in Hslot_lookup by congruence.
    destruct (Hcurrent query_cpu cpu) as [rq [index [entity Hresult]]].
    + unfold kernel_cpu_slot.
      split; auto.
    + exact Hcpu_has_current.
    + exists rq, index, entity.
      destruct Hresult as [Hactive [Hentity [Hstate [Htid Hgeneration]]]].
      split.
      * unfold kernel_active_runqueue, kernel_replace_cpu in *.
        simpl in *.
        exact Hactive.
      * split.
        -- exact Hentity.
        -- repeat split; auto.
Qed.

Lemma kernel_replace_cpu_preserves_running_entity_has_current :
  forall sched cpu_idx old_cpu new_cpu,
    kernel_sched_shape sched ->
    kernel_lookup_cpu sched cpu_idx = Some old_cpu ->
    kc_has_current new_cpu = kc_has_current old_cpu ->
    kc_current_thread_id new_cpu = kc_current_thread_id old_cpu ->
    kc_current_generation new_cpu = kc_current_generation old_cpu ->
    kernel_running_entity_has_current sched ->
    kernel_running_entity_has_current
      (kernel_replace_cpu sched cpu_idx new_cpu).
Proof.
  intros sched cpu_idx old_cpu new_cpu Hshape Hlookup
    Hhas Hthread Hgen Hrunning.
  unfold kernel_running_entity_has_current in *.
  intros run_cpu local_rq index entity Hactive Hentity Hstate.
  destruct (Hrunning run_cpu local_rq index entity Hactive Hentity Hstate)
    as [cpu [Hslot [Hcurrent [Htid Hgeneration]]]].
  destruct Hslot as [Hlt Hslot_lookup].
  destruct (Nat.eq_dec run_cpu cpu_idx) as [Heq | Hneq].
  - subst run_cpu.
    exists new_cpu.
    split.
    + unfold kernel_cpu_slot, kernel_replace_cpu.
      simpl.
      split; auto.
      rewrite nth_error_replace_nth_eq.
      * reflexivity.
      * destruct Hshape as [Hcount [_ Hcpus_len]].
        rewrite Hcpus_len.
        lia.
    + unfold kernel_lookup_cpu in Hlookup.
      rewrite Hslot_lookup in Hlookup.
      inversion Hlookup; subst old_cpu.
      rewrite Hhas.
      rewrite Hthread.
      rewrite Hgen.
      repeat split; auto.
  - exists cpu.
    split.
    + unfold kernel_cpu_slot, kernel_replace_cpu.
      simpl.
      split; auto.
      rewrite nth_error_replace_nth_neq by congruence.
      exact Hslot_lookup.
    + repeat split; auto.
Qed.

Lemma kernel_set_activation_pending_preserves_current_matches :
  forall sched cpu_idx pending,
    kernel_sched_shape sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_current_matches_local_running sched ->
    kernel_current_matches_local_running
      (kernel_set_activation_pending sched cpu_idx pending).
Proof.
  intros sched cpu_idx pending Hshape Hvalid Hcurrent.
  unfold kernel_set_activation_pending.
  destruct (kernel_lookup_cpu sched cpu_idx) as [old_cpu |] eqn:Hlookup;
    [| exact Hcurrent].
  destruct Hshape as [Hcount [_ Hcpus_len]].
  pose proof (kernel_valid_cpu_lt sched cpu_idx Hvalid) as Hvalid_lt.
  unfold kernel_current_matches_local_running in *.
  intros query_cpu cpu Hslot Hhas.
  unfold kernel_cpu_slot, kernel_replace_cpu in Hslot.
  simpl in Hslot.
  destruct Hslot as [Hlt Hslot_lookup].
  destruct (Nat.eq_dec query_cpu cpu_idx) as [Heq | Hneq].
  - subst query_cpu.
    rewrite nth_error_replace_nth_eq in Hslot_lookup.
    + inversion Hslot_lookup; subst cpu.
      apply (Hcurrent cpu_idx old_cpu).
      * unfold kernel_cpu_slot. split; auto.
      * simpl in Hhas.
        exact Hhas.
    + rewrite Hcpus_len.
      lia.
  - rewrite nth_error_replace_nth_neq in Hslot_lookup by congruence.
    apply (Hcurrent query_cpu cpu); auto.
    unfold kernel_cpu_slot. split; auto.
Qed.

Lemma kernel_set_activation_pending_preserves_activation_targets :
  forall sched cpu_idx pending,
    kernel_sched_shape sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_activation_targets_valid_cpus sched ->
    kernel_activation_targets_valid_cpus
      (kernel_set_activation_pending sched cpu_idx pending).
Proof.
  intros sched cpu_idx pending Hshape Hvalid Htargets.
  unfold kernel_set_activation_pending.
  destruct (kernel_lookup_cpu sched cpu_idx) as [old_cpu |] eqn:Hlookup;
    [| exact Htargets].
  unfold kernel_activation_targets_valid_cpus in *.
  intros query_cpu cpu Hcpu_lookup Hpending.
  unfold kernel_replace_cpu in Hcpu_lookup.
  simpl in Hcpu_lookup.
  destruct (Nat.eq_dec query_cpu cpu_idx) as [Heq | Hneq].
  - subst query_cpu.
    rewrite nth_error_replace_nth_eq in Hcpu_lookup.
    + apply kernel_valid_cpu_lt in Hvalid.
      exact Hvalid.
    + destruct Hshape as [Hcount [_ Hcpus_len]].
      apply kernel_valid_cpu_lt in Hvalid.
      rewrite Hcpus_len.
      lia.
  - rewrite nth_error_replace_nth_neq in Hcpu_lookup by congruence.
    apply Htargets with (cpu := cpu); auto.
Qed.

Lemma kernel_replace_cpu_preserves_activation_targets :
  forall sched cpu_idx cpu,
    kernel_sched_shape sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_activation_targets_valid_cpus sched ->
    kernel_activation_targets_valid_cpus
      (kernel_replace_cpu sched cpu_idx cpu).
Proof.
  intros sched cpu_idx replacement Hshape Hvalid Htargets.
  unfold kernel_activation_targets_valid_cpus in *.
  intros query_cpu cpu Hcpu_lookup Hpending.
  unfold kernel_replace_cpu in Hcpu_lookup.
  simpl in Hcpu_lookup.
  destruct (Nat.eq_dec query_cpu cpu_idx) as [Heq | Hneq].
  - subst query_cpu.
    rewrite nth_error_replace_nth_eq in Hcpu_lookup.
    + apply kernel_valid_cpu_lt in Hvalid.
      exact Hvalid.
    + destruct Hshape as [Hcount [_ Hcpus_len]].
      apply kernel_valid_cpu_lt in Hvalid.
      rewrite Hcpus_len.
      lia.
  - rewrite nth_error_replace_nth_neq in Hcpu_lookup by congruence.
    apply Htargets with (cpu := cpu); auto.
Qed.

Lemma kernel_replace_runqueue_preserves_activation_targets :
  forall sched cpu_idx rq,
    kernel_activation_targets_valid_cpus sched ->
    kernel_activation_targets_valid_cpus (kernel_replace_runqueue sched cpu_idx rq).
Proof.
  intros sched cpu_idx rq Htargets.
  unfold kernel_activation_targets_valid_cpus in *.
  intros query_cpu cpu Hlookup Hpending.
  unfold kernel_replace_runqueue in Hlookup.
  simpl in Hlookup.
  apply Htargets with (cpu := cpu); auto.
Qed.

Lemma kernel_replace_runqueue_preserves_no_cross_cpu_current_duplicates :
  forall sched cpu_idx rq,
    kernel_no_cross_cpu_current_duplicates sched ->
    kernel_no_cross_cpu_current_duplicates (kernel_replace_runqueue sched cpu_idx rq).
Proof.
  intros sched cpu_idx rq Hnodup.
  unfold kernel_no_cross_cpu_current_duplicates in *.
  intros cpu_a cpu_b lhs rhs Hslot_lhs Hslot_rhs Hhas_lhs Hhas_rhs Htid Heq.
  apply (Hnodup cpu_a cpu_b lhs rhs).
  - apply kernel_replace_runqueue_preserves_cpu_slot
      with (target_cpu := cpu_idx) (rq := rq).
    exact Hslot_lhs.
  - apply kernel_replace_runqueue_preserves_cpu_slot
      with (target_cpu := cpu_idx) (rq := rq).
    exact Hslot_rhs.
  - exact Hhas_lhs.
  - exact Hhas_rhs.
  - exact Htid.
  - exact Heq.
Qed.

Lemma kernel_clear_cpu_current_preserves_activation_targets :
  forall sched cpu_idx,
    kernel_sched_shape sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_activation_targets_valid_cpus sched ->
    kernel_activation_targets_valid_cpus
      (kernel_clear_cpu_current sched cpu_idx).
Proof.
  intros sched cpu_idx Hshape Hvalid Htargets.
  unfold kernel_clear_cpu_current.
  destruct (kernel_lookup_cpu sched cpu_idx) as [old_cpu |] eqn:Hlookup.
  - apply kernel_replace_cpu_preserves_activation_targets; auto.
  - exact Htargets.
Qed.

Lemma kernel_clear_cpu_current_preserves_no_cross_cpu_current_duplicates :
  forall sched cpu_idx,
    kernel_no_cross_cpu_current_duplicates sched ->
    kernel_no_cross_cpu_current_duplicates
      (kernel_clear_cpu_current sched cpu_idx).
Proof.
  intros sched cpu_idx Hnodup.
  unfold kernel_no_cross_cpu_current_duplicates in *.
  intros cpu_a cpu_b lhs rhs Hslot_lhs Hslot_rhs Hhas_lhs Hhas_rhs Htid Heq.
  destruct (Nat.eq_dec cpu_a cpu_idx) as [Ha | Hna];
    destruct (Nat.eq_dec cpu_b cpu_idx) as [Hb | Hnb].
  - subst. reflexivity.
  - subst cpu_a.
    unfold kernel_clear_cpu_current in Hslot_lhs.
    destruct (kernel_lookup_cpu sched cpu_idx) as [old_cpu |] eqn:Hlookup.
    + destruct Hslot_lhs as [Hlt_lhs Hlookup_lhs].
      unfold kernel_replace_cpu in Hlookup_lhs.
      simpl in Hlookup_lhs.
      rewrite nth_error_replace_nth_eq in Hlookup_lhs.
      * inversion Hlookup_lhs; subst lhs.
        simpl in Hhas_lhs.
        discriminate.
      * unfold kernel_lookup_cpu in Hlookup.
        apply nth_error_Some.
        rewrite Hlookup.
        discriminate.
    + apply (Hnodup cpu_idx cpu_b lhs rhs).
      * exact Hslot_lhs.
      * apply kernel_clear_cpu_current_preserves_other_cpu_slot
          with (target_cpu := cpu_idx); auto.
      * exact Hhas_lhs.
      * exact Hhas_rhs.
      * exact Htid.
      * exact Heq.
  - subst cpu_b.
    unfold kernel_clear_cpu_current in Hslot_rhs.
    destruct (kernel_lookup_cpu sched cpu_idx) as [old_cpu |] eqn:Hlookup.
    + destruct Hslot_rhs as [Hlt_rhs Hlookup_rhs].
      unfold kernel_replace_cpu in Hlookup_rhs.
      simpl in Hlookup_rhs.
      rewrite nth_error_replace_nth_eq in Hlookup_rhs.
      * inversion Hlookup_rhs; subst rhs.
        simpl in Hhas_rhs.
        discriminate.
      * unfold kernel_lookup_cpu in Hlookup.
        apply nth_error_Some.
        rewrite Hlookup.
        discriminate.
    + apply (Hnodup cpu_a cpu_idx lhs rhs).
      * apply kernel_clear_cpu_current_preserves_other_cpu_slot
          with (target_cpu := cpu_idx); auto.
      * exact Hslot_rhs.
      * exact Hhas_lhs.
      * exact Hhas_rhs.
      * exact Htid.
      * exact Heq.
  - apply (Hnodup cpu_a cpu_b lhs rhs).
    + apply kernel_clear_cpu_current_preserves_other_cpu_slot
        with (target_cpu := cpu_idx); auto.
    + apply kernel_clear_cpu_current_preserves_other_cpu_slot
        with (target_cpu := cpu_idx); auto.
    + exact Hhas_lhs.
    + exact Hhas_rhs.
    + exact Htid.
    + exact Heq.
Qed.

Lemma kernel_set_cpu_current_preserves_activation_targets :
  forall sched cpu_idx thread_id generation,
    kernel_sched_shape sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_activation_targets_valid_cpus sched ->
    kernel_activation_targets_valid_cpus
      (kernel_set_cpu_current sched cpu_idx thread_id generation).
Proof.
  intros sched cpu_idx thread_id generation Hshape Hvalid Htargets.
  unfold kernel_set_cpu_current.
  destruct (kernel_lookup_cpu sched cpu_idx) as [old_cpu |] eqn:Hlookup.
  - apply kernel_replace_cpu_preserves_activation_targets; auto.
  - exact Htargets.
Qed.

Theorem kernel_set_activation_pending_preserves_invariant :
  forall sched cpu_idx pending,
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_sched_invariant
      (kernel_set_activation_pending sched cpu_idx pending).
Proof.
  intros sched cpu_idx pending Hinv Hvalid.
  destruct Hinv as
    [Hshape
      [Hrqs
      [Hunique
      [Hcurrent
      [Hrunning
      [Hnodup Htargets]]]]]].
  destruct (kernel_lookup_cpu_some sched cpu_idx Hshape Hvalid)
    as [old_cpu Hlookup].
  unfold kernel_set_activation_pending.
  rewrite Hlookup.
  split.
  - apply kernel_replace_cpu_shape.
    exact Hshape.
  - split.
    + apply kernel_replace_cpu_preserves_runqueues_invariant.
      exact Hrqs.
    + split.
      * apply kernel_replace_cpu_preserves_global_thread_ids_unique.
        exact Hunique.
      * split.
        -- apply kernel_replace_cpu_preserves_current_matches
             with (old_cpu := old_cpu); auto.
        -- split.
           ++ apply kernel_replace_cpu_preserves_running_entity_has_current
                with (old_cpu := old_cpu); auto.
           ++ split.
              ** unfold kernel_no_cross_cpu_current_duplicates in *.
                 intros cpu_a cpu_b lhs rhs Hslot_lhs Hslot_rhs Hhas_lhs Hhas_rhs Htid Heq.
                 unfold kernel_cpu_slot, kernel_replace_cpu in *.
                 simpl in *.
                 destruct Hslot_lhs as [Hlt_lhs Hlookup_lhs].
                 destruct Hslot_rhs as [Hlt_rhs Hlookup_rhs].
                 destruct (Nat.eq_dec cpu_a cpu_idx) as [Ha | Hna];
                   destruct (Nat.eq_dec cpu_b cpu_idx) as [Hb | Hnb].
                 --- subst. reflexivity.
                 --- subst cpu_a.
                     rewrite nth_error_replace_nth_eq in Hlookup_lhs.
                     +++ inversion Hlookup_lhs; subst lhs.
                         rewrite nth_error_replace_nth_neq in Hlookup_rhs by congruence.
                         apply (Hnodup cpu_idx cpu_b old_cpu rhs).
                         *** unfold kernel_cpu_slot. split; auto.
                         *** unfold kernel_cpu_slot. split; auto.
                         *** exact Hhas_lhs.
                         *** exact Hhas_rhs.
                         *** exact Htid.
                         *** exact Heq.
                     +++ destruct Hshape as [Hcount [_ Hcpus_len]].
                         rewrite Hcpus_len. lia.
                 --- subst cpu_b.
                     rewrite nth_error_replace_nth_neq in Hlookup_lhs by congruence.
                     rewrite nth_error_replace_nth_eq in Hlookup_rhs.
                     +++ inversion Hlookup_rhs; subst rhs.
                         apply (Hnodup cpu_a cpu_idx lhs old_cpu).
                         *** unfold kernel_cpu_slot. split; auto.
                         *** unfold kernel_cpu_slot. split; auto.
                         *** exact Hhas_lhs.
                         *** exact Hhas_rhs.
                         *** exact Htid.
                         *** exact Heq.
                     +++ destruct Hshape as [Hcount [_ Hcpus_len]].
                         rewrite Hcpus_len. lia.
                 --- rewrite nth_error_replace_nth_neq in Hlookup_lhs by congruence.
                     rewrite nth_error_replace_nth_neq in Hlookup_rhs by congruence.
                     apply (Hnodup cpu_a cpu_b lhs rhs);
                       unfold kernel_cpu_slot; auto.
              ** apply kernel_replace_cpu_preserves_activation_targets; auto.
Qed.

Lemma eevdf_pick_none_returns_same_runqueue :
  forall rq rq_after_pick,
    eevdf_invariant rq ->
    eevdf_pick rq = (rq_after_pick, None) ->
    rq_after_pick = rq.
Proof.
  intros rq rq_after_pick Hinv Hpick.
  unfold eevdf_pick in Hpick.
  destruct (best_eligible rq (er_virtual_time rq)) as [[best_index best_entity] |]
    eqn:Hbest.
  - discriminate.
  - destruct (next_eligible rq) as [next_time |] eqn:Hnext.
    + pose proof (next_eligible_some_spec rq next_time Hnext)
        as [[future_index [future_entity [Hfuture_active [Hfuture_state Hfuture_time]]]]
          _].
      assert (exists picked, best_eligible
        {|
          er_entities := er_entities rq;
          er_entity_count := er_entity_count rq;
          er_runnable_count := er_runnable_count rq;
          er_virtual_time := next_time;
          er_min_vruntime := er_min_vruntime rq;
        |}
        next_time = Some picked).
      {
        apply best_eligible_exists_if_eligible with
          (index := future_index)
          (entity := future_entity).
        - simpl. exact Hfuture_active.
        - split; auto.
          lia.
      }
      destruct H as [[picked_index picked_entity] Hpicked].
      rewrite Hpicked in Hpick.
      discriminate.
    + inversion Hpick.
      reflexivity.
Qed.

Lemma eevdf_pick_some_returns_active_runnable :
  forall rq rq_after_pick index entity,
    eevdf_pick rq = (rq_after_pick, Some (index, entity)) ->
    eevdf_active_entity rq_after_pick index entity /\
    ee_state entity = ERunnable.
Proof.
  intros rq rq_after_pick index entity Hpick.
  unfold eevdf_pick in Hpick.
  destruct (best_eligible rq (er_virtual_time rq)) as [[best_index best_entity] |]
    eqn:Hbest.
  - inversion Hpick; subst rq_after_pick index entity.
    pose proof (best_eligible_some_spec
      rq
      (er_virtual_time rq)
      best_index
      best_entity
      Hbest) as [Hactive [[Hstate _] _]].
    split; auto.
  - destruct (next_eligible rq) as [next_time |] eqn:Hnext; try discriminate.
    destruct (best_eligible
      {|
        er_entities := er_entities rq;
        er_entity_count := er_entity_count rq;
        er_runnable_count := er_runnable_count rq;
        er_virtual_time := next_time;
        er_min_vruntime := er_min_vruntime rq;
      |}
      next_time) as [[best_index best_entity] |] eqn:Hbest_next;
      try discriminate.
    inversion Hpick; subst rq_after_pick index entity.
    pose proof (best_eligible_some_spec
      {|
        er_entities := er_entities rq;
        er_entity_count := er_entity_count rq;
        er_runnable_count := er_runnable_count rq;
        er_virtual_time := next_time;
        er_min_vruntime := er_min_vruntime rq;
      |}
      next_time
      best_index
      best_entity
      Hbest_next) as [Hactive [[Hstate _] _]].
    split; auto.
Qed.

Lemma eevdf_pick_some_returns_original_active_runnable :
  forall rq rq_after_pick index entity,
    eevdf_pick rq = (rq_after_pick, Some (index, entity)) ->
    eevdf_active_entity rq index entity /\
    ee_state entity = ERunnable.
Proof.
  intros rq rq_after_pick index entity Hpick.
  unfold eevdf_pick in Hpick.
  destruct (best_eligible rq (er_virtual_time rq)) as [[best_index best_entity] |]
    eqn:Hbest.
  - inversion Hpick; subst rq_after_pick index entity.
    pose proof (best_eligible_some_spec
      rq
      (er_virtual_time rq)
      best_index
      best_entity
      Hbest) as [Hactive [[Hstate _] _]].
    split; auto.
  - destruct (next_eligible rq) as [next_time |] eqn:Hnext; try discriminate.
    destruct (best_eligible
      {|
        er_entities := er_entities rq;
        er_entity_count := er_entity_count rq;
        er_runnable_count := er_runnable_count rq;
        er_virtual_time := next_time;
        er_min_vruntime := er_min_vruntime rq;
      |}
      next_time) as [[best_index best_entity] |] eqn:Hbest_next;
      try discriminate.
    inversion Hpick; subst rq_after_pick index entity.
    pose proof (best_eligible_some_spec
      {|
        er_entities := er_entities rq;
        er_entity_count := er_entity_count rq;
        er_runnable_count := er_runnable_count rq;
        er_virtual_time := next_time;
        er_min_vruntime := er_min_vruntime rq;
      |}
      next_time
      best_index
      best_entity
      Hbest_next) as [Hactive [[Hstate _] _]].
    simpl in Hactive.
    split; auto.
Qed.

Lemma eevdf_pick_preserves_active_entity_backward :
  forall rq rq_after_pick picked index entity,
    eevdf_pick rq = (rq_after_pick, picked) ->
    eevdf_active_entity rq_after_pick index entity ->
    eevdf_active_entity rq index entity.
Proof.
  intros rq rq_after_pick picked index entity Hpick Hactive.
  unfold eevdf_pick in Hpick.
  destruct (best_eligible rq (er_virtual_time rq)) as [[best_index best_entity] |]
    eqn:Hbest.
  - inversion Hpick; subst rq_after_pick picked.
    exact Hactive.
  - destruct (next_eligible rq) as [next_time |] eqn:Hnext.
    + destruct (best_eligible
        {|
          er_entities := er_entities rq;
          er_entity_count := er_entity_count rq;
          er_runnable_count := er_runnable_count rq;
          er_virtual_time := next_time;
          er_min_vruntime := er_min_vruntime rq;
        |}
        next_time) as [[picked_index picked_entity] |] eqn:Hbest_next;
        inversion Hpick; subst rq_after_pick picked;
        destruct Hactive as [Hlt Hlookup];
        split; simpl in *; auto.
    + inversion Hpick; subst rq_after_pick picked.
      exact Hactive.
Qed.

Lemma eevdf_mark_running_ok_thread_not_none :
  forall rq thread_id rq',
    eevdf_mark_running rq thread_id = ok rq' ->
    thread_id <> no_thread_id.
Proof.
  intros rq thread_id rq' Hmark Hthread.
  subst thread_id.
  unfold eevdf_mark_running in Hmark.
  unfold find_entity_index in Hmark.
  rewrite Z.eqb_refl in Hmark.
  discriminate.
Qed.

Lemma eevdf_mark_running_success_has_running_entity :
  forall rq thread_id rq',
    eevdf_invariant rq ->
    eevdf_mark_running rq thread_id = ok rq' ->
    exists index entity,
      eevdf_active_entity
        rq'
        index
        (set_entity_state entity ERunning) /\
      ee_state (set_entity_state entity ERunning) = ERunning /\
      ee_thread_id (set_entity_state entity ERunning) = ee_thread_id entity /\
      ee_generation (set_entity_state entity ERunning) = ee_generation entity.
Proof.
  intros rq thread_id rq' Hinv Hmark.
  destruct (eevdf_mark_running_success_formula rq thread_id rq' Hmark)
    as [index [entity [Hfind [_Hlookup [_Hstate Hrq']]]]].
  subst rq'.
  exists index, entity.
  split.
  - unfold eevdf_active_entity, recount_runqueue, replace_entity.
    simpl.
    split.
    + apply find_entity_index_lt with (thread_id := thread_id).
      exact Hfind.
    + apply nth_error_replace_nth_eq.
      destruct Hinv as [[Hcount Hlen] _].
      rewrite Hlen.
      pose proof (find_entity_index_lt rq thread_id index Hfind) as Hindex.
      lia.
  - repeat split; reflexivity.
Qed.

Lemma eevdf_mark_running_success_has_matching_running_entity :
  forall rq thread_id rq',
    eevdf_invariant rq ->
    eevdf_mark_running rq thread_id = ok rq' ->
    exists index entity,
      eevdf_active_entity rq' index entity /\
      ee_state entity = ERunning /\
      ee_thread_id entity = thread_id.
Proof.
  intros rq thread_id rq' Hinv Hmark.
  destruct (eevdf_mark_running_success_formula rq thread_id rq' Hmark)
    as [index [source [Hfind [Hlookup [_Hstate Hrq']]]]].
  pose proof (find_entity_index_lookup_matches_thread
    rq
    thread_id
    index
    source
    Hfind
    Hlookup) as [Hthread _].
  subst rq'.
  exists index, (set_entity_state source ERunning).
  split.
  - unfold eevdf_active_entity, recount_runqueue, replace_entity.
    simpl.
    split.
    + apply find_entity_index_lt with (thread_id := thread_id).
      exact Hfind.
    + apply nth_error_replace_nth_eq.
      destruct Hinv as [[Hcount Hlen] _].
      rewrite Hlen.
      pose proof (find_entity_index_lt rq thread_id index Hfind) as Hindex.
      lia.
  - split.
    + reflexivity.
    + rewrite set_entity_state_running_thread_id.
      exact Hthread.
Qed.

Lemma eevdf_mark_running_active_entity_original_thread :
  forall rq thread_id rq' index entity,
    eevdf_invariant rq ->
    eevdf_mark_running rq thread_id = ok rq' ->
    eevdf_active_entity rq' index entity ->
    is_active_state (ee_state entity) = true ->
    exists original,
      eevdf_active_entity rq index original /\
      is_active_state (ee_state original) = true /\
      ee_thread_id original = ee_thread_id entity.
Proof.
  intros rq thread_id rq' index entity Hinv Hmark Hactive Hactive_state.
  destruct (eevdf_mark_running_success_formula rq thread_id rq' Hmark)
    as [marked_index [marked_entity
      [Hfind [Hlookup [Hmarked_state Hrq']]]]].
  subst rq'.
  apply (proj1 (recount_active_entity_iff _ _ _)) in Hactive.
  apply replace_entity_active_cases_shaped in Hactive.
  - destruct Hactive as
      [[Hindex Hentity] | [Hneq Hactive_original]].
    + subst index entity.
      exists marked_entity.
      split.
      * apply find_entity_index_active with (thread_id := thread_id); auto.
      * split.
        -- rewrite Hmarked_state.
           reflexivity.
        -- rewrite set_entity_state_running_thread_id.
           reflexivity.
	    + exists entity.
	      split.
	      * exact Hactive_original.
	      * split; auto.
  - destruct Hinv as [Hshape _].
    exact Hshape.
Qed.

Lemma set_entity_state_running_generation :
  forall entity,
    ee_generation (set_entity_state entity ERunning) = ee_generation entity.
Proof.
  reflexivity.
Qed.

Lemma eevdf_mark_running_running_entity_cases :
  forall rq thread_id rq' index entity,
    eevdf_invariant rq ->
    eevdf_mark_running rq thread_id = ok rq' ->
    eevdf_active_entity rq' index entity ->
    ee_state entity = ERunning ->
    (exists source,
      eevdf_active_entity rq index source /\
      ee_state source = ERunnable /\
      ee_thread_id source = thread_id /\
      entity = set_entity_state source ERunning) \/
    (exists original,
      eevdf_active_entity rq index original /\
      ee_state original = ERunning /\
      ee_thread_id original = ee_thread_id entity /\
      ee_generation original = ee_generation entity).
Proof.
  intros rq thread_id rq' index entity Hinv Hmark Hactive Hrunning.
  destruct (eevdf_mark_running_success_formula rq thread_id rq' Hmark)
    as [marked_index [marked_entity
      [Hfind [Hlookup [Hsource_state Hrq']]]]].
  pose proof (find_entity_index_lookup_matches_thread
    rq
    thread_id
    marked_index
    marked_entity
    Hfind
    Hlookup) as [Hsource_thread _Hsource_active].
  subst rq'.
  apply (proj1 (recount_active_entity_iff _ _ _)) in Hactive.
  apply replace_entity_active_cases_shaped in Hactive.
  - destruct Hactive as [[Hindex Hentity] | [Hneq Hactive_original]].
    + subst index entity.
      left.
      exists marked_entity.
      split.
      * apply find_entity_index_active with (thread_id := thread_id); auto.
      * split.
        -- exact Hsource_state.
        -- split.
           ++ exact Hsource_thread.
           ++ reflexivity.
    + right.
      exists entity.
      split.
      * exact Hactive_original.
      * split.
        -- exact Hrunning.
        -- split; reflexivity.
  - destruct Hinv as [Hshape _].
    exact Hshape.
Qed.

Lemma eevdf_pick_mark_running_matches_picked_entity :
  forall rq rq_after_pick picked_index picked_entity rq',
    eevdf_invariant rq ->
    eevdf_pick rq = (rq_after_pick, Some (picked_index, picked_entity)) ->
    eevdf_mark_running rq_after_pick (ee_thread_id picked_entity) = ok rq' ->
    exists run_index running_entity,
      eevdf_active_entity rq' run_index running_entity /\
      ee_state running_entity = ERunning /\
      ee_thread_id running_entity = ee_thread_id picked_entity /\
      ee_generation running_entity = ee_generation picked_entity.
Proof.
  intros rq rq_after_pick picked_index picked_entity rq'
    Hinv Hpick Hmark.
  pose proof (pick_preserves_invariant
    rq
    rq_after_pick
    (Some (picked_index, picked_entity))
    Hinv
    Hpick) as Hafter_inv.
  pose proof (eevdf_pick_some_returns_active_runnable
    rq
    rq_after_pick
    picked_index
    picked_entity
    Hpick) as [Hpicked_active Hpicked_state].
  destruct (eevdf_mark_running_success_formula
    rq_after_pick
    (ee_thread_id picked_entity)
    rq'
    Hmark) as [run_index [source [Hfind [Hlookup [Hsource_state Hrq']]]]].
  pose proof (find_entity_index_active
    rq_after_pick
    (ee_thread_id picked_entity)
    run_index
    source
    Hfind
    Hlookup) as Hsource_active.
  pose proof (find_entity_index_lookup_matches_thread
    rq_after_pick
    (ee_thread_id picked_entity)
    run_index
    source
    Hfind
    Hlookup) as [Hsource_thread Hsource_active_state].
  assert (Hpicked_thread_not_none : ee_thread_id picked_entity <> no_thread_id).
  {
    unfold find_entity_index in Hfind.
    destruct (ee_thread_id picked_entity =? no_thread_id) eqn:Hno;
      try discriminate.
    apply Z.eqb_neq in Hno.
    exact Hno.
  }
  destruct Hafter_inv as
    [_Hshape
      [_Hinactive
      [Hunique_after
      [_Hpositive
      [_Hlive
      [_Hmin
      [_Hvirtual _Hrunnable]]]]]]].
  assert (Hsame_index : run_index = picked_index).
  {
    apply Hunique_after with (lhs := source) (rhs := picked_entity).
    - exact Hsource_active.
    - exact Hpicked_active.
    - rewrite Hsource_thread.
      exact Hpicked_thread_not_none.
    - exact Hsource_thread.
    - exact Hsource_active_state.
    - rewrite Hpicked_state.
      reflexivity.
  }
  subst run_index.
  destruct Hsource_active as [_ Hsource_lookup].
  destruct Hpicked_active as [_ Hpicked_lookup].
  rewrite Hsource_lookup in Hpicked_lookup.
  inversion Hpicked_lookup; subst source.
  subst rq'.
  exists picked_index, (set_entity_state picked_entity ERunning).
  split.
  - unfold eevdf_active_entity, recount_runqueue, replace_entity.
    simpl.
    split.
    + apply find_entity_index_lt with (thread_id := ee_thread_id picked_entity).
      exact Hfind.
    + apply nth_error_replace_nth_eq.
      destruct _Hshape as [Hcount Hlen].
      rewrite Hlen.
      pose proof (find_entity_index_lt
        rq_after_pick
        (ee_thread_id picked_entity)
        picked_index
        Hfind) as Hindex.
      lia.
  - repeat split; reflexivity.
Qed.

Lemma eevdf_pick_mark_running_running_entity_cases :
  forall rq rq_after_pick picked_index picked_entity rq' index entity,
    eevdf_invariant rq ->
    eevdf_pick rq = (rq_after_pick, Some (picked_index, picked_entity)) ->
    eevdf_mark_running rq_after_pick (ee_thread_id picked_entity) = ok rq' ->
    eevdf_active_entity rq' index entity ->
    ee_state entity = ERunning ->
    (ee_thread_id entity = ee_thread_id picked_entity /\
      ee_generation entity = ee_generation picked_entity) \/
    (exists original,
      eevdf_active_entity rq index original /\
      ee_state original = ERunning /\
      ee_thread_id original = ee_thread_id entity /\
      ee_generation original = ee_generation entity).
Proof.
  intros rq rq_after_pick picked_index picked_entity rq' index entity
    Hinv Hpick Hmark Hactive Hrunning.
  pose proof (pick_preserves_invariant
    rq
    rq_after_pick
    (Some (picked_index, picked_entity))
    Hinv
    Hpick) as Hafter_inv.
  pose proof (eevdf_pick_some_returns_active_runnable
    rq
    rq_after_pick
    picked_index
    picked_entity
    Hpick) as [Hpicked_active Hpicked_state].
  destruct (eevdf_mark_running_running_entity_cases
    rq_after_pick
    (ee_thread_id picked_entity)
    rq'
    index
    entity
    Hafter_inv
    Hmark
    Hactive
    Hrunning) as
    [[source [Hsource_active [Hsource_state [Hsource_thread Hentity]]]]
    | [original [Horiginal_active [Horiginal_state [Horiginal_thread Horiginal_generation]]]]].
  - left.
    assert (Hpicked_thread_not_none : ee_thread_id picked_entity <> no_thread_id).
    {
      apply eevdf_mark_running_ok_thread_not_none with
        (rq := rq_after_pick)
        (rq' := rq').
      exact Hmark.
    }
    destruct Hafter_inv as
      [_Hshape
        [_Hinactive
        [Hunique_after
        [_Hpositive
        [_Hlive
        [_Hmin
        [_Hvirtual _Hrunnable]]]]]]].
    assert (Hsame_index : index = picked_index).
    {
      apply Hunique_after with (lhs := source) (rhs := picked_entity).
      - exact Hsource_active.
      - exact Hpicked_active.
      - rewrite Hsource_thread.
        exact Hpicked_thread_not_none.
      - exact Hsource_thread.
      - rewrite Hsource_state.
        reflexivity.
      - rewrite Hpicked_state.
        reflexivity.
    }
    subst index.
    destruct Hsource_active as [_ Hsource_lookup].
    destruct Hpicked_active as [_ Hpicked_lookup].
    rewrite Hsource_lookup in Hpicked_lookup.
    inversion Hpicked_lookup; subst source.
    subst entity.
    split.
    + rewrite set_entity_state_running_thread_id.
      reflexivity.
    + rewrite set_entity_state_running_generation.
      reflexivity.
  - right.
    exists original.
    split.
    + apply eevdf_pick_preserves_active_entity_backward
        with
          (rq_after_pick := rq_after_pick)
          (picked := Some (picked_index, picked_entity)); auto.
    + split.
      * exact Horiginal_state.
      * split; auto.
Qed.

Theorem kernel_pick_run_thread_success_sets_current :
  forall sched cpu_idx rq_after_pick picked_entity rq',
    kernel_sched_shape sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    exists cpu,
      kernel_cpu_slot
        (kernel_set_cpu_current
          (kernel_set_activation_pending
            (kernel_replace_runqueue
              (kernel_replace_runqueue sched cpu_idx rq_after_pick)
              cpu_idx
              rq')
            cpu_idx
            false)
          cpu_idx
          (ee_thread_id picked_entity)
          (ee_generation picked_entity))
        cpu_idx
        cpu /\
      kc_has_current cpu = true /\
      kc_current_thread_id cpu = ee_thread_id picked_entity /\
      kc_current_generation cpu = ee_generation picked_entity.
Proof.
  intros sched cpu_idx rq_after_pick picked_entity rq' Hshape Hvalid.
  destruct (kernel_lookup_cpu_some sched cpu_idx Hshape Hvalid)
    as [old_cpu Hold_lookup].
  set (sched_rq :=
    kernel_replace_runqueue
      (kernel_replace_runqueue sched cpu_idx rq_after_pick)
      cpu_idx
      rq').
  assert (Hshape_rq : kernel_sched_shape sched_rq).
  {
    unfold sched_rq.
    repeat apply kernel_replace_runqueue_shape.
    exact Hshape.
  }
  assert (Hvalid_rq : kernel_valid_cpu sched_rq cpu_idx = true).
  {
    unfold sched_rq, kernel_valid_cpu.
    simpl.
    exact Hvalid.
  }
  assert (Hlookup_rq : kernel_lookup_cpu sched_rq cpu_idx = Some old_cpu).
  {
    unfold sched_rq, kernel_lookup_cpu, kernel_replace_runqueue.
    simpl.
    unfold kernel_lookup_cpu in Hold_lookup.
    exact Hold_lookup.
  }
  set (pending_cpu :=
    {|
      kc_has_current := kc_has_current old_cpu;
      kc_current_thread_id := kc_current_thread_id old_cpu;
      kc_current_generation := kc_current_generation old_cpu;
      kc_activation_pending := false;
    |}).
  assert (Hlookup_pending :
    kernel_lookup_cpu
      (kernel_set_activation_pending sched_rq cpu_idx false)
      cpu_idx =
      Some pending_cpu).
  {
    unfold pending_cpu.
    apply kernel_set_activation_pending_lookup_cpu_eq; auto.
  }
  pose proof (kernel_set_activation_pending_shape
    sched_rq
    cpu_idx
    false
    Hshape_rq) as Hshape_pending.
  assert (Hvalid_pending :
    kernel_valid_cpu
      (kernel_set_activation_pending sched_rq cpu_idx false)
      cpu_idx = true).
  {
    unfold kernel_valid_cpu.
    rewrite kernel_set_activation_pending_preserves_cpu_count.
    exact Hvalid_rq.
  }
  exists
    {|
      kc_has_current := true;
      kc_current_thread_id := ee_thread_id picked_entity;
      kc_current_generation := ee_generation picked_entity;
      kc_activation_pending := kc_activation_pending pending_cpu;
    |}.
  split.
  - apply kernel_set_cpu_current_sets_cpu_slot with
      (old_cpu := pending_cpu); auto.
  - repeat split; reflexivity.
Qed.

Theorem kernel_pick_run_thread_success_target_slot_current :
  forall sched cpu_idx rq_after_pick picked_entity rq' cpu,
    kernel_sched_shape sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_cpu_slot
      (kernel_set_cpu_current
        (kernel_set_activation_pending
          (kernel_replace_runqueue
            (kernel_replace_runqueue sched cpu_idx rq_after_pick)
            cpu_idx
            rq')
          cpu_idx
          false)
        cpu_idx
        (ee_thread_id picked_entity)
        (ee_generation picked_entity))
      cpu_idx
      cpu ->
    kc_has_current cpu = true /\
    kc_current_thread_id cpu = ee_thread_id picked_entity /\
    kc_current_generation cpu = ee_generation picked_entity.
Proof.
  intros sched cpu_idx rq_after_pick picked_entity rq' cpu Hshape Hvalid Hslot.
  destruct (kernel_pick_run_thread_success_sets_current
    sched
    cpu_idx
    rq_after_pick
    picked_entity
    rq'
    Hshape
    Hvalid) as [picked_cpu [Hpicked_slot Hpicked_current]].
  destruct Hslot as [_ Hslot_lookup].
  destruct Hpicked_slot as [_ Hpicked_lookup].
  rewrite Hslot_lookup in Hpicked_lookup.
  inversion Hpicked_lookup; subst picked_cpu.
  exact Hpicked_current.
Qed.

Lemma kernel_pick_run_thread_success_other_cpu_slot_original :
  forall sched cpu_idx rq_after_pick picked_entity rq' query_cpu cpu,
    query_cpu <> cpu_idx ->
    kernel_cpu_slot
      (kernel_set_cpu_current
        (kernel_set_activation_pending
          (kernel_replace_runqueue
            (kernel_replace_runqueue sched cpu_idx rq_after_pick)
            cpu_idx
            rq')
          cpu_idx
          false)
        cpu_idx
        (ee_thread_id picked_entity)
        (ee_generation picked_entity))
      query_cpu
      cpu ->
    kernel_cpu_slot sched query_cpu cpu.
Proof.
  intros sched cpu_idx rq_after_pick picked_entity rq' query_cpu cpu Hneq Hslot.
  pose proof (kernel_set_cpu_current_preserves_other_cpu_slot
    (kernel_set_activation_pending
      (kernel_replace_runqueue
        (kernel_replace_runqueue sched cpu_idx rq_after_pick)
        cpu_idx rq')
      cpu_idx false)
    cpu_idx
    query_cpu
    (ee_thread_id picked_entity)
    (ee_generation picked_entity)
    cpu
    Hneq
    Hslot) as Hslot_pending.
  pose proof (kernel_set_activation_pending_preserves_other_cpu_slot
    (kernel_replace_runqueue
      (kernel_replace_runqueue sched cpu_idx rq_after_pick)
      cpu_idx rq')
    cpu_idx
    query_cpu
    false
    cpu
    Hneq
    Hslot_pending) as Hslot_after_rq.
  pose proof (kernel_replace_runqueue_preserves_cpu_slot
    (kernel_replace_runqueue sched cpu_idx rq_after_pick)
    cpu_idx
    rq'
    query_cpu
    cpu
    Hslot_after_rq) as Hslot_after_pick.
  exact (kernel_replace_runqueue_preserves_cpu_slot
    sched
    cpu_idx
    rq_after_pick
    query_cpu
    cpu
    Hslot_after_pick).
Qed.

Lemma kernel_pick_run_thread_success_other_cpu_slot_final :
  forall sched cpu_idx rq_after_pick picked_entity rq' query_cpu cpu,
    query_cpu <> cpu_idx ->
    kernel_cpu_slot sched query_cpu cpu ->
    kernel_cpu_slot
      (kernel_set_cpu_current
        (kernel_set_activation_pending
          (kernel_replace_runqueue
            (kernel_replace_runqueue sched cpu_idx rq_after_pick)
            cpu_idx
            rq')
          cpu_idx
          false)
        cpu_idx
        (ee_thread_id picked_entity)
        (ee_generation picked_entity))
      query_cpu
      cpu.
Proof.
  intros sched cpu_idx rq_after_pick picked_entity rq' query_cpu cpu Hneq Hslot.
  eapply kernel_set_cpu_current_preserves_other_cpu_slot_forward.
  - exact Hneq.
  - eapply kernel_set_activation_pending_preserves_other_cpu_slot_forward.
    + exact Hneq.
    + apply kernel_replace_runqueue_preserves_cpu_slot_forward.
      apply kernel_replace_runqueue_preserves_cpu_slot_forward.
      exact Hslot.
Qed.

Theorem kernel_pick_run_thread_success_preserves_shape :
  forall sched cpu_idx rq_after_pick picked_entity rq',
    kernel_sched_shape sched ->
    kernel_sched_shape
      (kernel_set_cpu_current
        (kernel_set_activation_pending
          (kernel_replace_runqueue
            (kernel_replace_runqueue sched cpu_idx rq_after_pick)
            cpu_idx
            rq')
          cpu_idx
          false)
        cpu_idx
        (ee_thread_id picked_entity)
        (ee_generation picked_entity)).
Proof.
  intros sched cpu_idx rq_after_pick picked_entity rq' Hshape.
  apply kernel_set_cpu_current_shape.
  apply kernel_set_activation_pending_shape.
  apply kernel_replace_runqueue_shape.
  apply kernel_replace_runqueue_shape.
  exact Hshape.
Qed.

Theorem kernel_pick_run_thread_success_current_matches_picked_cpu :
  forall sched cpu_idx rq rq_after_pick picked_index picked_entity rq',
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_lookup_runqueue sched cpu_idx = Some rq ->
    eevdf_pick rq = (rq_after_pick, Some (picked_index, picked_entity)) ->
    eevdf_mark_running rq_after_pick (ee_thread_id picked_entity) = ok rq' ->
    exists cpu run_index running_entity,
      kernel_cpu_slot
        (kernel_set_cpu_current
          (kernel_set_activation_pending
            (kernel_replace_runqueue
              (kernel_replace_runqueue sched cpu_idx rq_after_pick)
              cpu_idx
              rq')
            cpu_idx
            false)
          cpu_idx
          (ee_thread_id picked_entity)
          (ee_generation picked_entity))
        cpu_idx
        cpu /\
      kc_has_current cpu = true /\
      kernel_entity_on_cpu
        (kernel_set_cpu_current
          (kernel_set_activation_pending
            (kernel_replace_runqueue
              (kernel_replace_runqueue sched cpu_idx rq_after_pick)
              cpu_idx
              rq')
            cpu_idx
            false)
          cpu_idx
          (ee_thread_id picked_entity)
          (ee_generation picked_entity))
        cpu_idx
        run_index
        running_entity /\
      ee_state running_entity = ERunning /\
      ee_thread_id running_entity = kc_current_thread_id cpu /\
      ee_generation running_entity = kc_current_generation cpu.
Proof.
  intros sched cpu_idx rq rq_after_pick picked_index picked_entity rq'
    Hinv Hvalid Hlookup Hpick Hmark.
  destruct Hinv as
    [Hshape
      [Hrqs
      [Hunique
      [Hcurrent
      [Hrunning
      [Hnodup Htargets]]]]]].
  pose proof Hshape as Hshape_for_current.
  destruct Hshape as [Hcpu_count_le [Hrqs_len Hcpus_len]].
  assert (Hactive_rq : kernel_active_runqueue sched cpu_idx rq).
  {
    unfold kernel_active_runqueue.
    split.
    - apply kernel_valid_cpu_lt. exact Hvalid.
    - unfold kernel_lookup_runqueue in Hlookup. exact Hlookup.
  }
  pose proof (Hrqs cpu_idx rq Hactive_rq) as Hrq_inv.
  destruct (eevdf_pick_mark_running_matches_picked_entity
    rq
    rq_after_pick
    picked_index
    picked_entity
    rq'
    Hrq_inv
    Hpick
    Hmark) as
    [run_index
      [running_entity
        [Hrunning_active [Hrunning_state [Hrunning_tid Hrunning_gen]]]]].
  destruct (kernel_pick_run_thread_success_sets_current
    sched
    cpu_idx
    rq_after_pick
    picked_entity
    rq'
    Hshape_for_current
    Hvalid) as
    [cpu [Hcpu_slot [Hcpu_has [Hcpu_tid Hcpu_gen]]]].
  exists cpu, run_index, running_entity.
  split.
  - exact Hcpu_slot.
  - split.
    + exact Hcpu_has.
    + split.
      * unfold kernel_entity_on_cpu.
        exists rq'.
        split.
        -- unfold kernel_active_runqueue.
           split.
           ++ simpl.
              rewrite kernel_set_cpu_current_preserves_cpu_count.
              rewrite kernel_set_activation_pending_preserves_cpu_count.
              repeat rewrite kernel_replace_runqueue_preserves_cpu_count.
              apply kernel_valid_cpu_lt.
              exact Hvalid.
           ++ simpl.
              rewrite kernel_set_cpu_current_preserves_runqueues.
              rewrite kernel_set_activation_pending_preserves_runqueues.
              unfold kernel_replace_runqueue.
              simpl.
              apply nth_error_replace_nth_eq.
              rewrite replace_nth_length.
              rewrite Hrqs_len.
              pose proof (kernel_valid_cpu_lt sched cpu_idx Hvalid) as Hcpu_lt.
              lia.
        -- split.
           ++ exact Hrunning_active.
           ++ rewrite Hrunning_state.
              reflexivity.
      * repeat split.
        -- exact Hrunning_state.
        -- rewrite Hrunning_tid.
           rewrite Hcpu_tid.
           reflexivity.
        -- rewrite Hrunning_gen.
           rewrite Hcpu_gen.
           reflexivity.
Qed.

Theorem kernel_pick_run_thread_success_rq_preserves_invariant :
  forall sched cpu_idx rq rq_after_pick picked_index picked_entity rq',
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_lookup_runqueue sched cpu_idx = Some rq ->
    eevdf_pick rq = (rq_after_pick, Some (picked_index, picked_entity)) ->
    eevdf_mark_running rq_after_pick (ee_thread_id picked_entity) = ok rq' ->
    eevdf_invariant rq'.
Proof.
  intros sched cpu_idx rq rq_after_pick picked_index picked_entity rq'
    Hinv Hvalid Hlookup Hpick Hmark.
  destruct Hinv as [Hshape [Hrqs _]].
  assert (Hactive_rq : kernel_active_runqueue sched cpu_idx rq).
  {
    unfold kernel_active_runqueue.
    split.
    - apply kernel_valid_cpu_lt. exact Hvalid.
    - unfold kernel_lookup_runqueue in Hlookup. exact Hlookup.
  }
  pose proof (Hrqs cpu_idx rq Hactive_rq) as Hrq_inv.
  pose proof (pick_preserves_invariant
    rq
    rq_after_pick
    (Some (picked_index, picked_entity))
    Hrq_inv
    Hpick) as Hafter_pick.
  apply mark_running_preserves_invariant
    with (rq := rq_after_pick)
         (thread_id := ee_thread_id picked_entity); auto.
Qed.

Theorem kernel_pick_run_thread_success_selected_runqueue_invariant :
  forall sched cpu_idx rq rq_after_pick picked_index picked_entity rq',
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_lookup_runqueue sched cpu_idx = Some rq ->
    eevdf_pick rq = (rq_after_pick, Some (picked_index, picked_entity)) ->
    eevdf_mark_running rq_after_pick (ee_thread_id picked_entity) = ok rq' ->
    let final_sched :=
      kernel_set_cpu_current
        (kernel_set_activation_pending
          (kernel_replace_runqueue
            (kernel_replace_runqueue sched cpu_idx rq_after_pick)
            cpu_idx
            rq')
          cpu_idx
          false)
        cpu_idx
        (ee_thread_id picked_entity)
        (ee_generation picked_entity) in
    kernel_active_runqueue final_sched cpu_idx rq' /\
    eevdf_invariant rq'.
Proof.
  intros sched cpu_idx rq rq_after_pick picked_index picked_entity rq'
    Hinv Hvalid Hlookup Hpick Hmark final_sched.
  destruct Hinv as
    [Hshape
      [Hrqs
      [Hunique
      [Hcurrent
      [Hrunning
      [Hnodup Htargets]]]]]].
  pose proof Hshape as Hshape_for_inv.
  destruct Hshape as [Hcpu_count_le [Hrqs_len Hcpus_len]].
  split.
  - unfold final_sched, kernel_active_runqueue.
    split.
    + simpl.
      rewrite kernel_set_cpu_current_preserves_cpu_count.
      rewrite kernel_set_activation_pending_preserves_cpu_count.
      repeat rewrite kernel_replace_runqueue_preserves_cpu_count.
      apply kernel_valid_cpu_lt.
      exact Hvalid.
    + simpl.
      rewrite kernel_set_cpu_current_preserves_runqueues.
      rewrite kernel_set_activation_pending_preserves_runqueues.
      unfold kernel_replace_runqueue.
      simpl.
      apply nth_error_replace_nth_eq.
      rewrite replace_nth_length.
      rewrite Hrqs_len.
      pose proof (kernel_valid_cpu_lt sched cpu_idx Hvalid) as Hcpu_lt.
      lia.
  - apply kernel_pick_run_thread_success_rq_preserves_invariant
      with (sched := {|
        ks_runqueues := ks_runqueues sched;
        ks_cpus := ks_cpus sched;
        ks_cpu_count := ks_cpu_count sched;
        ks_balance_cursor := ks_balance_cursor sched;
      |})
      (cpu_idx := cpu_idx)
      (rq := rq)
      (rq_after_pick := rq_after_pick)
      (picked_index := picked_index)
      (picked_entity := picked_entity); simpl; auto.
    exact
      (conj Hshape_for_inv
        (conj Hrqs
        (conj Hunique
        (conj Hcurrent
        (conj Hrunning
        (conj Hnodup Htargets)))))).
Qed.

Theorem kernel_pick_run_thread_success_preserves_runqueues_invariant :
  forall sched cpu_idx rq rq_after_pick picked_index picked_entity rq',
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_lookup_runqueue sched cpu_idx = Some rq ->
    eevdf_pick rq = (rq_after_pick, Some (picked_index, picked_entity)) ->
    eevdf_mark_running rq_after_pick (ee_thread_id picked_entity) = ok rq' ->
    let final_sched :=
      kernel_set_cpu_current
        (kernel_set_activation_pending
          (kernel_replace_runqueue
            (kernel_replace_runqueue sched cpu_idx rq_after_pick)
            cpu_idx
            rq')
          cpu_idx
          false)
        cpu_idx
        (ee_thread_id picked_entity)
        (ee_generation picked_entity) in
    kernel_runqueues_invariant final_sched.
Proof.
  intros sched cpu_idx rq rq_after_pick picked_index picked_entity rq'
    Hinv Hvalid Hlookup Hpick Hmark final_sched.
  destruct Hinv as
    [Hshape
      [Hrqs
      [Hunique
      [Hcurrent
      [Hrunning
      [Hnodup Htargets]]]]]].
  pose proof
    (conj Hshape
      (conj Hrqs
      (conj Hunique
      (conj Hcurrent
      (conj Hrunning
      (conj Hnodup Htargets)))))) as Hinv_full.
  unfold kernel_runqueues_invariant.
  intros query_cpu local_rq Hactive_final.
  destruct (Nat.eq_dec query_cpu cpu_idx) as [Heq | Hneq].
  - subst query_cpu.
    destruct (kernel_pick_run_thread_success_selected_runqueue_invariant
      sched
      cpu_idx
      rq
      rq_after_pick
      picked_index
      picked_entity
      rq'
      Hinv_full
      Hvalid
      Hlookup
      Hpick
      Hmark) as [Hselected_active Hrq_inv].
    destruct Hactive_final as [_ Hlookup_final].
    destruct Hselected_active as [_ Hlookup_selected].
    unfold final_sched in Hlookup_final.
    rewrite Hlookup_final in Hlookup_selected.
    inversion Hlookup_selected; subst local_rq.
    exact Hrq_inv.
  - apply (Hrqs query_cpu local_rq).
    destruct Hactive_final as [Hlt_final Hlookup_final].
    unfold kernel_active_runqueue.
    split.
    + unfold final_sched in Hlt_final.
      simpl in Hlt_final.
      rewrite kernel_set_cpu_current_preserves_cpu_count in Hlt_final.
      rewrite kernel_set_activation_pending_preserves_cpu_count in Hlt_final.
      repeat rewrite kernel_replace_runqueue_preserves_cpu_count in Hlt_final.
      exact Hlt_final.
    + unfold final_sched in Hlookup_final.
      simpl in Hlookup_final.
      rewrite kernel_set_cpu_current_preserves_runqueues in Hlookup_final.
      rewrite kernel_set_activation_pending_preserves_runqueues in Hlookup_final.
      unfold kernel_replace_runqueue in Hlookup_final.
      simpl in Hlookup_final.
      rewrite nth_error_replace_nth_neq in Hlookup_final by congruence.
      rewrite nth_error_replace_nth_neq in Hlookup_final by congruence.
      exact Hlookup_final.
Qed.

Theorem kernel_pick_run_thread_success_running_has_current_on_picked_cpu :
  forall sched cpu_idx rq rq_after_pick picked_index picked_entity rq',
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_lookup_runqueue sched cpu_idx = Some rq ->
    eevdf_pick rq = (rq_after_pick, Some (picked_index, picked_entity)) ->
    eevdf_mark_running rq_after_pick (ee_thread_id picked_entity) = ok rq' ->
    exists cpu run_index running_entity,
      kernel_cpu_slot
        (kernel_set_cpu_current
          (kernel_set_activation_pending
            (kernel_replace_runqueue
              (kernel_replace_runqueue sched cpu_idx rq_after_pick)
              cpu_idx
              rq')
            cpu_idx
            false)
          cpu_idx
          (ee_thread_id picked_entity)
          (ee_generation picked_entity))
        cpu_idx
        cpu /\
      kernel_entity_on_cpu
        (kernel_set_cpu_current
          (kernel_set_activation_pending
            (kernel_replace_runqueue
              (kernel_replace_runqueue sched cpu_idx rq_after_pick)
              cpu_idx
              rq')
            cpu_idx
            false)
          cpu_idx
          (ee_thread_id picked_entity)
          (ee_generation picked_entity))
        cpu_idx
        run_index
        running_entity /\
      ee_state running_entity = ERunning /\
      kc_has_current cpu = true /\
      kc_current_thread_id cpu = ee_thread_id running_entity /\
      kc_current_generation cpu = ee_generation running_entity.
Proof.
  intros sched cpu_idx rq rq_after_pick picked_index picked_entity rq'
    Hinv Hvalid Hlookup Hpick Hmark.
  destruct (kernel_pick_run_thread_success_current_matches_picked_cpu
    sched
    cpu_idx
    rq
    rq_after_pick
    picked_index
    picked_entity
    rq'
    Hinv
    Hvalid
    Hlookup
    Hpick
    Hmark) as [cpu [run_index [running_entity Hparts]]].
  destruct Hparts as
    [Hcpu_slot [Hcpu_has [Hentity [Hstate [Hthread Hgeneration]]]]].
  exists cpu, run_index, running_entity.
  split.
  - exact Hcpu_slot.
  - split.
    + exact Hentity.
    + split.
      * exact Hstate.
      * split.
        -- exact Hcpu_has.
        -- split.
           ++ symmetry.
              exact Hthread.
           ++ symmetry.
              exact Hgeneration.
Qed.

Theorem kernel_pick_run_thread_success_preserves_current_matches :
  forall sched cpu_idx rq rq_after_pick picked_index picked_entity rq',
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_cpu_has_current sched cpu_idx = false ->
    kernel_lookup_runqueue sched cpu_idx = Some rq ->
    eevdf_pick rq = (rq_after_pick, Some (picked_index, picked_entity)) ->
    eevdf_mark_running rq_after_pick (ee_thread_id picked_entity) = ok rq' ->
    let final_sched :=
      kernel_set_cpu_current
        (kernel_set_activation_pending
          (kernel_replace_runqueue
            (kernel_replace_runqueue sched cpu_idx rq_after_pick)
            cpu_idx
            rq')
          cpu_idx
          false)
        cpu_idx
        (ee_thread_id picked_entity)
        (ee_generation picked_entity) in
    kernel_current_matches_local_running final_sched.
Proof.
  intros sched cpu_idx rq rq_after_pick picked_index picked_entity rq'
    Hinv Hvalid Hcpu_idle Hlookup Hpick Hmark.
  set (final_sched :=
    kernel_set_cpu_current
      (kernel_set_activation_pending
        (kernel_replace_runqueue
          (kernel_replace_runqueue sched cpu_idx rq_after_pick)
          cpu_idx
          rq')
        cpu_idx
        false)
      cpu_idx
      (ee_thread_id picked_entity)
      (ee_generation picked_entity)).
  destruct Hinv as
    [Hshape
      [Hrqs
      [Hunique
      [Hcurrent
      [Hrunning
      [Hnodup Htargets]]]]]].
  pose proof
    (conj Hshape
      (conj Hrqs
      (conj Hunique
      (conj Hcurrent
      (conj Hrunning
      (conj Hnodup Htargets)))))) as Hinv_full.
  unfold kernel_current_matches_local_running.
  intros query_cpu cpu Hslot Hhas_current.
  destruct (Nat.eq_dec query_cpu cpu_idx) as [Heq | Hneq].
  - subst query_cpu.
    destruct (kernel_pick_run_thread_success_current_matches_picked_cpu
      sched
      cpu_idx
      rq
      rq_after_pick
      picked_index
      picked_entity
      rq'
      Hinv_full
      Hvalid
      Hlookup
      Hpick
      Hmark) as [picked_cpu [run_index [running_entity Hparts]]].
    destruct Hparts as
      [Hpicked_slot
      [Hpicked_has
      [Hentity [Hstate [Hthread Hgeneration]]]]].
    destruct Hslot as [_ Hslot_lookup].
    destruct Hpicked_slot as [_ Hpicked_lookup].
    unfold final_sched in Hslot_lookup.
    unfold final_sched in Hpicked_lookup.
    rewrite Hslot_lookup in Hpicked_lookup.
    inversion Hpicked_lookup; subst picked_cpu.
    destruct Hentity as [entity_rq [Hactive [Hentity_active _Hactive_state]]].
    exists entity_rq, run_index, running_entity.
    exact
      (conj Hactive
        (conj Hentity_active
          (conj Hstate
            (conj Hthread Hgeneration)))).
  - assert (Hslot_original : kernel_cpu_slot sched query_cpu cpu).
    {
      unfold final_sched in Hslot.
      pose proof (kernel_set_cpu_current_preserves_other_cpu_slot
        (kernel_set_activation_pending
          (kernel_replace_runqueue
            (kernel_replace_runqueue sched cpu_idx rq_after_pick)
            cpu_idx rq')
          cpu_idx false)
        cpu_idx
        query_cpu
        (ee_thread_id picked_entity)
        (ee_generation picked_entity)
        cpu
        Hneq
        Hslot) as Hslot_pending.
      pose proof (kernel_set_activation_pending_preserves_other_cpu_slot
        (kernel_replace_runqueue
          (kernel_replace_runqueue sched cpu_idx rq_after_pick)
          cpu_idx rq')
        cpu_idx
        query_cpu
        false
        cpu
        Hneq
        Hslot_pending) as Hslot_after_rq.
      pose proof (kernel_replace_runqueue_preserves_cpu_slot
        (kernel_replace_runqueue sched cpu_idx rq_after_pick)
        cpu_idx
        rq'
        query_cpu
        cpu
        Hslot_after_rq) as Hslot_after_pick.
      exact (kernel_replace_runqueue_preserves_cpu_slot
        sched
        cpu_idx
        rq_after_pick
        query_cpu
        cpu
        Hslot_after_pick).
    }
    destruct (Hcurrent query_cpu cpu Hslot_original Hhas_current)
      as [old_rq [old_index [old_entity
        [Hold_active [Hold_entity [Hold_state [Hold_thread Hold_gen]]]]]]].
    assert (Hold_active_final : kernel_active_runqueue final_sched query_cpu old_rq).
    {
      destruct Hold_active as [Hold_lt Hold_lookup].
      unfold kernel_active_runqueue.
      split.
      - unfold final_sched.
        simpl.
        rewrite kernel_set_cpu_current_preserves_cpu_count.
        rewrite kernel_set_activation_pending_preserves_cpu_count.
        repeat rewrite kernel_replace_runqueue_preserves_cpu_count.
        exact Hold_lt.
      - unfold final_sched.
        simpl.
        rewrite kernel_set_cpu_current_preserves_runqueues.
        rewrite kernel_set_activation_pending_preserves_runqueues.
        unfold kernel_replace_runqueue.
        simpl.
        rewrite nth_error_replace_nth_neq by congruence.
        rewrite nth_error_replace_nth_neq by congruence.
        exact Hold_lookup.
    }
    exists old_rq, old_index, old_entity.
	    exact
	      (conj Hold_active_final
	        (conj Hold_entity
	          (conj Hold_state
	            (conj Hold_thread Hold_gen)))).
Qed.

Theorem kernel_pick_run_thread_success_preserves_running_entity_has_current :
  forall sched cpu_idx rq rq_after_pick picked_index picked_entity rq',
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_cpu_has_current sched cpu_idx = false ->
    kernel_lookup_runqueue sched cpu_idx = Some rq ->
    eevdf_pick rq = (rq_after_pick, Some (picked_index, picked_entity)) ->
    eevdf_mark_running rq_after_pick (ee_thread_id picked_entity) = ok rq' ->
    let final_sched :=
      kernel_set_cpu_current
        (kernel_set_activation_pending
          (kernel_replace_runqueue
            (kernel_replace_runqueue sched cpu_idx rq_after_pick)
            cpu_idx
            rq')
          cpu_idx
          false)
        cpu_idx
        (ee_thread_id picked_entity)
        (ee_generation picked_entity) in
    kernel_running_entity_has_current final_sched.
Proof.
  intros sched cpu_idx rq rq_after_pick picked_index picked_entity rq'
    Hinv Hvalid Hcpu_idle Hlookup Hpick Hmark.
  set (final_sched :=
    kernel_set_cpu_current
      (kernel_set_activation_pending
        (kernel_replace_runqueue
          (kernel_replace_runqueue sched cpu_idx rq_after_pick)
          cpu_idx
          rq')
        cpu_idx
        false)
      cpu_idx
      (ee_thread_id picked_entity)
      (ee_generation picked_entity)).
  destruct Hinv as
    [Hshape
      [Hrqs
      [Hunique
      [Hcurrent
      [Hrunning
      [Hnodup Htargets]]]]]].
  pose proof
    (conj Hshape
      (conj Hrqs
      (conj Hunique
      (conj Hcurrent
      (conj Hrunning
      (conj Hnodup Htargets)))))) as Hinv_full.
  assert (Hrq_active : kernel_active_runqueue sched cpu_idx rq).
  {
    unfold kernel_active_runqueue.
    split.
    - apply kernel_valid_cpu_lt.
      exact Hvalid.
    - unfold kernel_lookup_runqueue in Hlookup.
      exact Hlookup.
  }
  pose proof (Hrqs cpu_idx rq Hrq_active) as Hrq_inv.
  unfold kernel_running_entity_has_current.
  intros run_cpu local_rq index entity Hactive_final Hentity_active Hstate.
  destruct (Nat.eq_dec run_cpu cpu_idx) as [Heq | Hneq].
  - subst run_cpu.
    destruct (kernel_pick_run_thread_success_selected_runqueue_invariant
      sched
      cpu_idx
      rq
      rq_after_pick
      picked_index
      picked_entity
      rq'
      Hinv_full
      Hvalid
      Hlookup
      Hpick
      Hmark) as [Hselected_active Hrq_final_inv].
    destruct Hactive_final as [_ Hfinal_lookup].
    destruct Hselected_active as [_ Hselected_lookup].
    unfold final_sched in Hfinal_lookup.
    rewrite Hfinal_lookup in Hselected_lookup.
    inversion Hselected_lookup; subst local_rq.
    destruct (eevdf_pick_mark_running_running_entity_cases
      rq
      rq_after_pick
      picked_index
      picked_entity
      rq'
      index
      entity
      Hrq_inv
      Hpick
      Hmark
      Hentity_active
      Hstate) as
      [[Hthread Hgeneration] |
        [original [Horiginal_active
          [Horiginal_state [_Horiginal_thread _Horiginal_generation]]]]].
    + destruct (kernel_pick_run_thread_success_sets_current
        sched
        cpu_idx
        rq_after_pick
        picked_entity
        rq'
        Hshape
        Hvalid) as [cpu [Hslot [Hhas [Htid Hgen]]]].
      exists cpu.
      split.
      * unfold final_sched.
        exact Hslot.
      * split.
        -- exact Hhas.
        -- split.
           ++ rewrite Hthread.
              exact Htid.
           ++ rewrite Hgeneration.
              exact Hgen.
    + destruct (Hrunning cpu_idx rq index original
        Hrq_active
        Horiginal_active
        Horiginal_state) as [old_cpu [Hold_slot [Hold_has _Hold_current]]].
      unfold kernel_cpu_has_current in Hcpu_idle.
      rewrite Hvalid in Hcpu_idle.
      destruct Hold_slot as [_ Hold_lookup].
      unfold kernel_lookup_cpu in Hcpu_idle.
      rewrite Hold_lookup in Hcpu_idle.
      rewrite Hold_has in Hcpu_idle.
      discriminate.
  - assert (Hactive_original : kernel_active_runqueue sched run_cpu local_rq).
    {
      destruct Hactive_final as [Hlt_final Hlookup_final].
      unfold kernel_active_runqueue.
      split.
      - unfold final_sched in Hlt_final.
        simpl in Hlt_final.
        rewrite kernel_set_cpu_current_preserves_cpu_count in Hlt_final.
        rewrite kernel_set_activation_pending_preserves_cpu_count in Hlt_final.
        repeat rewrite kernel_replace_runqueue_preserves_cpu_count in Hlt_final.
        exact Hlt_final.
      - unfold final_sched in Hlookup_final.
        simpl in Hlookup_final.
        rewrite kernel_set_cpu_current_preserves_runqueues in Hlookup_final.
        rewrite kernel_set_activation_pending_preserves_runqueues in Hlookup_final.
        unfold kernel_replace_runqueue in Hlookup_final.
        simpl in Hlookup_final.
        rewrite nth_error_replace_nth_neq in Hlookup_final by congruence.
        rewrite nth_error_replace_nth_neq in Hlookup_final by congruence.
        exact Hlookup_final.
    }
    destruct (Hrunning run_cpu local_rq index entity
      Hactive_original
      Hentity_active
      Hstate) as [cpu [Hslot [Hhas [Htid Hgen]]]].
    exists cpu.
    split.
    + unfold final_sched.
      apply kernel_pick_run_thread_success_other_cpu_slot_final; auto.
    + repeat split; auto.
Qed.

Theorem kernel_pick_run_thread_success_preserves_activation_targets :
  forall sched cpu_idx rq_after_pick picked_entity rq',
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    let final_sched :=
      kernel_set_cpu_current
        (kernel_set_activation_pending
          (kernel_replace_runqueue
            (kernel_replace_runqueue sched cpu_idx rq_after_pick)
            cpu_idx
            rq')
          cpu_idx
          false)
        cpu_idx
        (ee_thread_id picked_entity)
        (ee_generation picked_entity) in
    kernel_activation_targets_valid_cpus final_sched.
Proof.
  intros sched cpu_idx rq_after_pick picked_entity rq' Hinv Hvalid final_sched.
  destruct Hinv as
    [Hshape
      [Hrqs
      [Hunique
      [Hcurrent
      [Hrunning
      [Hnodup Htargets]]]]]].
  set (sched_rq :=
    kernel_replace_runqueue
      (kernel_replace_runqueue sched cpu_idx rq_after_pick)
      cpu_idx
      rq').
  assert (Hshape_rq : kernel_sched_shape sched_rq).
  {
    unfold sched_rq.
    repeat apply kernel_replace_runqueue_shape.
    exact Hshape.
  }
  assert (Hvalid_rq : kernel_valid_cpu sched_rq cpu_idx = true).
  {
    unfold sched_rq, kernel_valid_cpu.
    simpl.
    exact Hvalid.
  }
  assert (Htargets_rq : kernel_activation_targets_valid_cpus sched_rq).
  {
    unfold sched_rq.
    repeat apply kernel_replace_runqueue_preserves_activation_targets.
    exact Htargets.
  }
  assert (Htargets_pending :
    kernel_activation_targets_valid_cpus
      (kernel_set_activation_pending sched_rq cpu_idx false)).
  {
    apply kernel_set_activation_pending_preserves_activation_targets; auto.
  }
  unfold final_sched.
  apply kernel_set_cpu_current_preserves_activation_targets.
  - apply kernel_set_activation_pending_shape.
    exact Hshape_rq.
  - unfold kernel_valid_cpu.
    rewrite kernel_set_activation_pending_preserves_cpu_count.
    exact Hvalid_rq.
  - exact Htargets_pending.
Qed.

Theorem kernel_pick_run_thread_success_picked_current_unique :
  forall sched cpu_idx rq rq_after_pick picked_index picked_entity rq'
      query_cpu query_cpu_state,
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_cpu_has_current sched cpu_idx = false ->
    kernel_lookup_runqueue sched cpu_idx = Some rq ->
    eevdf_pick rq = (rq_after_pick, Some (picked_index, picked_entity)) ->
    eevdf_mark_running rq_after_pick (ee_thread_id picked_entity) = ok rq' ->
    let final_sched :=
      kernel_set_cpu_current
        (kernel_set_activation_pending
          (kernel_replace_runqueue
            (kernel_replace_runqueue sched cpu_idx rq_after_pick)
            cpu_idx
            rq')
          cpu_idx
          false)
        cpu_idx
        (ee_thread_id picked_entity)
        (ee_generation picked_entity) in
    kernel_cpu_slot final_sched query_cpu query_cpu_state ->
    kc_has_current query_cpu_state = true ->
    kc_current_thread_id query_cpu_state = ee_thread_id picked_entity ->
    query_cpu = cpu_idx.
Proof.
  intros sched cpu_idx rq rq_after_pick picked_index picked_entity rq'
    query_cpu query_cpu_state Hinv Hvalid Hcpu_idle Hlookup Hpick Hmark
    final_sched Hslot_final Hhas_current Hsame_thread.
  destruct (Nat.eq_dec query_cpu cpu_idx) as [Heq | Hneq]; [exact Heq |].
  destruct Hinv as
    [Hshape
      [Hrqs
      [Hunique
      [Hcurrent
      [Hrunning
      [Hnodup Htargets]]]]]].
  assert (Hslot_original : kernel_cpu_slot sched query_cpu query_cpu_state).
  {
    unfold final_sched in Hslot_final.
    pose proof (kernel_set_cpu_current_preserves_other_cpu_slot
      (kernel_set_activation_pending
        (kernel_replace_runqueue
          (kernel_replace_runqueue sched cpu_idx rq_after_pick)
          cpu_idx rq')
        cpu_idx false)
      cpu_idx
      query_cpu
      (ee_thread_id picked_entity)
      (ee_generation picked_entity)
      query_cpu_state
      Hneq
      Hslot_final) as Hslot_pending.
    pose proof (kernel_set_activation_pending_preserves_other_cpu_slot
      (kernel_replace_runqueue
        (kernel_replace_runqueue sched cpu_idx rq_after_pick)
        cpu_idx rq')
      cpu_idx
      query_cpu
      false
      query_cpu_state
      Hneq
      Hslot_pending) as Hslot_after_rq.
    pose proof (kernel_replace_runqueue_preserves_cpu_slot
      (kernel_replace_runqueue sched cpu_idx rq_after_pick)
      cpu_idx
      rq'
      query_cpu
      query_cpu_state
      Hslot_after_rq) as Hslot_after_pick.
    exact (kernel_replace_runqueue_preserves_cpu_slot
      sched
      cpu_idx
      rq_after_pick
      query_cpu
      query_cpu_state
      Hslot_after_pick).
  }
  destruct (Hcurrent query_cpu query_cpu_state Hslot_original Hhas_current)
    as [other_rq [other_index [other_entity
      [Hother_active [Hother_entity [Hother_state [Hother_thread _Hother_gen]]]]]]].
  assert (Hpicked_entity_on_cpu :
    kernel_entity_on_cpu sched cpu_idx picked_index picked_entity).
  {
    exists rq.
    split.
    - unfold kernel_active_runqueue.
      split.
      + apply kernel_valid_cpu_lt.
        exact Hvalid.
      + unfold kernel_lookup_runqueue in Hlookup.
        exact Hlookup.
    - pose proof (eevdf_pick_some_returns_original_active_runnable
        rq
        rq_after_pick
        picked_index
        picked_entity
        Hpick) as [Hpicked_active Hpicked_state].
      split.
      + exact Hpicked_active.
      + rewrite Hpicked_state.
        reflexivity.
  }
  assert (Hother_entity_on_cpu :
    kernel_entity_on_cpu sched query_cpu other_index other_entity).
  {
    exists other_rq.
    split.
    - exact Hother_active.
    - split.
      + exact Hother_entity.
      + rewrite Hother_state.
        reflexivity.
  }
  assert (Hpicked_not_none : ee_thread_id picked_entity <> no_thread_id).
  {
    eapply eevdf_mark_running_ok_thread_not_none.
    exact Hmark.
  }
  destruct (Hunique
    cpu_idx
    picked_index
    query_cpu
    other_index
    picked_entity
    other_entity
    Hpicked_entity_on_cpu
    Hother_entity_on_cpu
    Hpicked_not_none) as [Hcpu_eq _].
  - rewrite Hother_thread.
    symmetry.
    exact Hsame_thread.
  - symmetry.
    exact Hcpu_eq.
Qed.

Theorem kernel_pick_run_thread_success_preserves_no_cross_cpu_current_duplicates :
  forall sched cpu_idx rq rq_after_pick picked_index picked_entity rq',
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_cpu_has_current sched cpu_idx = false ->
    kernel_lookup_runqueue sched cpu_idx = Some rq ->
    eevdf_pick rq = (rq_after_pick, Some (picked_index, picked_entity)) ->
    eevdf_mark_running rq_after_pick (ee_thread_id picked_entity) = ok rq' ->
    let final_sched :=
      kernel_set_cpu_current
        (kernel_set_activation_pending
          (kernel_replace_runqueue
            (kernel_replace_runqueue sched cpu_idx rq_after_pick)
            cpu_idx
            rq')
          cpu_idx
          false)
        cpu_idx
        (ee_thread_id picked_entity)
        (ee_generation picked_entity) in
    kernel_no_cross_cpu_current_duplicates final_sched.
Proof.
  intros sched cpu_idx rq rq_after_pick picked_index picked_entity rq'
    Hinv Hvalid Hcpu_idle Hlookup Hpick Hmark final_sched.
  destruct Hinv as
    [Hshape
      [Hrqs
      [Hunique
      [Hcurrent
      [Hrunning
      [Hnodup Htargets]]]]]].
  pose proof
    (conj Hshape
      (conj Hrqs
      (conj Hunique
      (conj Hcurrent
      (conj Hrunning
      (conj Hnodup Htargets)))))) as Hinv_full.
  unfold kernel_no_cross_cpu_current_duplicates.
  intros cpu_a cpu_b lhs rhs Hslot_lhs Hslot_rhs Hhas_lhs Hhas_rhs Htid Heq.
  destruct (Nat.eq_dec cpu_a cpu_idx) as [Ha | Hna];
    destruct (Nat.eq_dec cpu_b cpu_idx) as [Hb | Hnb].
  - subst. reflexivity.
  - subst cpu_a.
    destruct (kernel_pick_run_thread_success_target_slot_current
      sched cpu_idx rq_after_pick picked_entity rq' lhs Hshape Hvalid) as
      [_ [Hlhs_tid _]].
    + unfold final_sched in Hslot_lhs.
      exact Hslot_lhs.
    + assert (Hrhs_picked :
        kc_current_thread_id rhs = ee_thread_id picked_entity).
      {
        rewrite <- Heq.
        exact Hlhs_tid.
      }
      symmetry.
      apply kernel_pick_run_thread_success_picked_current_unique
        with
          (sched := sched)
          (rq := rq)
          (rq_after_pick := rq_after_pick)
          (picked_index := picked_index)
          (picked_entity := picked_entity)
          (rq' := rq')
          (query_cpu_state := rhs); auto.
  - subst cpu_b.
    destruct (kernel_pick_run_thread_success_target_slot_current
      sched cpu_idx rq_after_pick picked_entity rq' rhs Hshape Hvalid) as
      [_ [Hrhs_tid _]].
    + unfold final_sched in Hslot_rhs.
      exact Hslot_rhs.
    + assert (Hlhs_picked :
        kc_current_thread_id lhs = ee_thread_id picked_entity).
      {
        rewrite Heq.
        exact Hrhs_tid.
      }
      apply kernel_pick_run_thread_success_picked_current_unique
        with
          (sched := sched)
          (rq := rq)
          (rq_after_pick := rq_after_pick)
          (picked_index := picked_index)
          (picked_entity := picked_entity)
          (rq' := rq')
          (query_cpu_state := lhs); auto.
  - apply (Hnodup cpu_a cpu_b lhs rhs).
    + apply kernel_pick_run_thread_success_other_cpu_slot_original
        with
          (sched := sched)
          (cpu_idx := cpu_idx)
          (rq_after_pick := rq_after_pick)
          (picked_entity := picked_entity)
          (query_cpu := cpu_a)
          (rq' := rq'); auto.
    + apply kernel_pick_run_thread_success_other_cpu_slot_original
        with
          (sched := sched)
          (cpu_idx := cpu_idx)
          (rq_after_pick := rq_after_pick)
          (picked_entity := picked_entity)
          (query_cpu := cpu_b)
          (rq' := rq'); auto.
    + exact Hhas_lhs.
    + exact Hhas_rhs.
    + exact Htid.
    + exact Heq.
Qed.

Theorem kernel_pick_run_thread_success_entity_on_cpu_original_thread :
  forall sched cpu_idx rq rq_after_pick picked_index picked_entity rq'
      entity_cpu entity_index entity,
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_lookup_runqueue sched cpu_idx = Some rq ->
    eevdf_pick rq = (rq_after_pick, Some (picked_index, picked_entity)) ->
    eevdf_mark_running rq_after_pick (ee_thread_id picked_entity) = ok rq' ->
    let final_sched :=
      kernel_set_cpu_current
        (kernel_set_activation_pending
          (kernel_replace_runqueue
            (kernel_replace_runqueue sched cpu_idx rq_after_pick)
            cpu_idx
            rq')
          cpu_idx
          false)
        cpu_idx
        (ee_thread_id picked_entity)
        (ee_generation picked_entity) in
    kernel_entity_on_cpu final_sched entity_cpu entity_index entity ->
    exists original,
      kernel_entity_on_cpu sched entity_cpu entity_index original /\
      ee_thread_id original = ee_thread_id entity.
Proof.
  intros sched cpu_idx rq rq_after_pick picked_index picked_entity rq'
    entity_cpu entity_index entity Hinv Hvalid Hlookup Hpick Hmark
    final_sched Hentity_final.
  destruct Hinv as
    [Hshape
      [Hrqs
      [Hunique
      [Hcurrent
      [Hrunning
      [Hnodup Htargets]]]]]].
  pose proof
    (conj Hshape
      (conj Hrqs
      (conj Hunique
      (conj Hcurrent
      (conj Hrunning
      (conj Hnodup Htargets)))))) as Hinv_full.
  destruct Hentity_final as
    [final_rq [Hactive_final [Hentity_active_final Hstate_active_final]]].
  destruct (Nat.eq_dec entity_cpu cpu_idx) as [Heq | Hneq].
  - subst entity_cpu.
    destruct (kernel_pick_run_thread_success_selected_runqueue_invariant
      sched
      cpu_idx
      rq
      rq_after_pick
      picked_index
      picked_entity
      rq'
      Hinv_full
      Hvalid
      Hlookup
      Hpick
      Hmark) as [Hselected_active Hrq_inv].
    destruct Hactive_final as [_ Hfinal_lookup].
    destruct Hselected_active as [_ Hselected_lookup].
    unfold final_sched in Hfinal_lookup.
    rewrite Hfinal_lookup in Hselected_lookup.
    inversion Hselected_lookup; subst final_rq.
    assert (Hrq_active : kernel_active_runqueue sched cpu_idx rq).
    {
      unfold kernel_active_runqueue.
      split.
      - apply kernel_valid_cpu_lt.
        exact Hvalid.
      - unfold kernel_lookup_runqueue in Hlookup.
        exact Hlookup.
    }
    pose proof (Hrqs cpu_idx rq Hrq_active) as Hrq_original_inv.
    pose proof (pick_preserves_invariant
      rq
      rq_after_pick
      (Some (picked_index, picked_entity))
      Hrq_original_inv
      Hpick) as Hrq_after_pick_inv.
    destruct (eevdf_mark_running_active_entity_original_thread
      rq_after_pick
      (ee_thread_id picked_entity)
      rq'
      entity_index
      entity
      Hrq_after_pick_inv
      Hmark
      Hentity_active_final
      Hstate_active_final) as
      [before_mark [Hbefore_mark_active [Hbefore_mark_state Hthread]]].
    exists before_mark.
    split.
    + exists rq.
      split.
      * exact Hrq_active.
      * split.
        -- apply eevdf_pick_preserves_active_entity_backward
             with
               (rq_after_pick := rq_after_pick)
               (picked := Some (picked_index, picked_entity)); auto.
        -- exact Hbefore_mark_state.
    + exact Hthread.
  - exists entity.
    split.
    + destruct Hactive_final as [Hlt_final Hlookup_final].
      exists final_rq.
      split.
      * unfold kernel_active_runqueue.
        split.
        -- unfold final_sched in Hlt_final.
           simpl in Hlt_final.
           rewrite kernel_set_cpu_current_preserves_cpu_count in Hlt_final.
           rewrite kernel_set_activation_pending_preserves_cpu_count in Hlt_final.
           repeat rewrite kernel_replace_runqueue_preserves_cpu_count in Hlt_final.
           exact Hlt_final.
        -- unfold final_sched in Hlookup_final.
           simpl in Hlookup_final.
           rewrite kernel_set_cpu_current_preserves_runqueues in Hlookup_final.
           rewrite kernel_set_activation_pending_preserves_runqueues in Hlookup_final.
           unfold kernel_replace_runqueue in Hlookup_final.
           simpl in Hlookup_final.
           rewrite nth_error_replace_nth_neq in Hlookup_final by congruence.
           rewrite nth_error_replace_nth_neq in Hlookup_final by congruence.
           exact Hlookup_final.
      * split; auto.
    + reflexivity.
Qed.

Theorem kernel_pick_run_thread_success_preserves_global_thread_ids_unique :
  forall sched cpu_idx rq rq_after_pick picked_index picked_entity rq',
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_lookup_runqueue sched cpu_idx = Some rq ->
    eevdf_pick rq = (rq_after_pick, Some (picked_index, picked_entity)) ->
    eevdf_mark_running rq_after_pick (ee_thread_id picked_entity) = ok rq' ->
    let final_sched :=
      kernel_set_cpu_current
        (kernel_set_activation_pending
          (kernel_replace_runqueue
            (kernel_replace_runqueue sched cpu_idx rq_after_pick)
            cpu_idx
            rq')
          cpu_idx
          false)
        cpu_idx
        (ee_thread_id picked_entity)
        (ee_generation picked_entity) in
    kernel_global_thread_ids_unique final_sched.
Proof.
  intros sched cpu_idx rq rq_after_pick picked_index picked_entity rq'
    Hinv Hvalid Hlookup Hpick Hmark final_sched.
  destruct Hinv as
    [Hshape
      [Hrqs
      [Hunique
      [Hcurrent
      [Hrunning
      [Hnodup Htargets]]]]]].
  pose proof
    (conj Hshape
      (conj Hrqs
      (conj Hunique
      (conj Hcurrent
      (conj Hrunning
      (conj Hnodup Htargets)))))) as Hinv_full.
  unfold kernel_global_thread_ids_unique.
  intros cpu_a index_a cpu_b index_b lhs rhs Hlhs Hrhs Hthread Heq.
  destruct (kernel_pick_run_thread_success_entity_on_cpu_original_thread
    sched
    cpu_idx
    rq
    rq_after_pick
    picked_index
    picked_entity
    rq'
    cpu_a
    index_a
    lhs
    Hinv_full
    Hvalid
    Hlookup
    Hpick
    Hmark
    Hlhs) as [orig_lhs [Horig_lhs Horig_lhs_thread]].
  destruct (kernel_pick_run_thread_success_entity_on_cpu_original_thread
    sched
    cpu_idx
    rq
    rq_after_pick
    picked_index
    picked_entity
    rq'
    cpu_b
    index_b
    rhs
    Hinv_full
    Hvalid
    Hlookup
    Hpick
    Hmark
    Hrhs) as [orig_rhs [Horig_rhs Horig_rhs_thread]].
  apply Hunique with (lhs := orig_lhs) (rhs := orig_rhs); auto.
  - rewrite Horig_lhs_thread.
    exact Hthread.
  - rewrite Horig_lhs_thread.
    rewrite Horig_rhs_thread.
    exact Heq.
Qed.

Theorem kernel_pick_run_thread_success_has_running_entity :
  forall sched cpu_idx rq rq_after_pick picked_index picked_entity rq',
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_lookup_runqueue sched cpu_idx = Some rq ->
    eevdf_pick rq = (rq_after_pick, Some (picked_index, picked_entity)) ->
    eevdf_mark_running rq_after_pick (ee_thread_id picked_entity) = ok rq' ->
    exists run_index source_entity,
      kernel_entity_on_cpu
        (kernel_set_cpu_current
          (kernel_set_activation_pending
            (kernel_replace_runqueue
              (kernel_replace_runqueue sched cpu_idx rq_after_pick)
              cpu_idx
              rq')
            cpu_idx
            false)
          cpu_idx
          (ee_thread_id picked_entity)
          (ee_generation picked_entity))
        cpu_idx
        run_index
        (set_entity_state source_entity ERunning) /\
      ee_state (set_entity_state source_entity ERunning) = ERunning.
Proof.
  intros sched cpu_idx rq rq_after_pick picked_index picked_entity rq'
    Hinv Hvalid Hlookup Hpick Hmark.
  destruct Hinv as
    [Hshape
      [Hrqs
      [Hunique
      [Hcurrent
      [Hrunning
      [Hnodup Htargets]]]]]].
  destruct Hshape as [Hcpu_count_le [Hrqs_len Hcpus_len]].
  assert (Hactive_rq : kernel_active_runqueue sched cpu_idx rq).
  {
    unfold kernel_active_runqueue.
    split.
    - apply kernel_valid_cpu_lt. exact Hvalid.
    - unfold kernel_lookup_runqueue in Hlookup. exact Hlookup.
  }
  pose proof (Hrqs cpu_idx rq Hactive_rq) as Hrq_inv.
  pose proof (pick_preserves_invariant
    rq
    rq_after_pick
    (Some (picked_index, picked_entity))
    Hrq_inv
    Hpick) as Hrq_after_pick_inv.
  destruct (eevdf_mark_running_success_has_running_entity
    rq_after_pick
    (ee_thread_id picked_entity)
    rq'
    Hrq_after_pick_inv
    Hmark) as [run_index [source_entity [Hrunning_entity Hstate]]].
  exists run_index, source_entity.
  split.
  - unfold kernel_entity_on_cpu.
    exists rq'.
    split.
    + unfold kernel_active_runqueue.
      split.
      * simpl.
        rewrite kernel_set_cpu_current_preserves_cpu_count.
        rewrite kernel_set_activation_pending_preserves_cpu_count.
        repeat rewrite kernel_replace_runqueue_preserves_cpu_count.
        apply kernel_valid_cpu_lt. exact Hvalid.
      * simpl.
        rewrite kernel_set_cpu_current_preserves_runqueues.
        rewrite kernel_set_activation_pending_preserves_runqueues.
        unfold kernel_replace_runqueue.
        simpl.
        apply nth_error_replace_nth_eq.
        rewrite replace_nth_length.
        rewrite Hrqs_len.
        pose proof (kernel_valid_cpu_lt sched cpu_idx Hvalid) as Hcpu_lt.
        lia.
    + split.
      * exact Hrunning_entity.
      * reflexivity.
  - destruct Hstate as [Hstate_only _].
    exact Hstate_only.
Qed.

Theorem kernel_request_activation_success_preserves_invariant :
  forall sched cpu_idx,
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_sched_invariant
      (fst (kernel_request_activation sched cpu_idx)).
Proof.
  intros sched cpu_idx Hinv Hvalid.
  unfold kernel_request_activation.
  rewrite Hvalid.
  simpl.
  destruct Hinv as
    [Hshape
      [Hrqs
      [Hunique
      [Hcurrent
      [Hrunning
      [Hnodup Htargets]]]]]].
  destruct (kernel_lookup_cpu_some sched cpu_idx Hshape Hvalid)
    as [old_cpu Hlookup].
  unfold kernel_set_activation_pending.
  rewrite Hlookup.
  split.
  - apply kernel_replace_cpu_shape.
    exact Hshape.
  - split.
    + apply kernel_replace_cpu_preserves_runqueues_invariant.
      exact Hrqs.
    + split.
      * apply kernel_replace_cpu_preserves_global_thread_ids_unique.
        exact Hunique.
      * split.
        -- apply kernel_replace_cpu_preserves_current_matches
             with (old_cpu := old_cpu); auto.
        -- split.
           ++ apply kernel_replace_cpu_preserves_running_entity_has_current
                with (old_cpu := old_cpu); auto.
           ++ split.
              ** unfold kernel_no_cross_cpu_current_duplicates in *.
                 intros cpu_a cpu_b lhs rhs Hslot_lhs Hslot_rhs Hhas_lhs Hhas_rhs Htid Heq.
                 unfold kernel_cpu_slot, kernel_replace_cpu in *.
                 simpl in *.
                 destruct Hslot_lhs as [Hlt_lhs Hlookup_lhs].
                 destruct Hslot_rhs as [Hlt_rhs Hlookup_rhs].
                 destruct (Nat.eq_dec cpu_a cpu_idx) as [Ha | Hna];
                   destruct (Nat.eq_dec cpu_b cpu_idx) as [Hb | Hnb].
                 --- subst. reflexivity.
                 --- subst cpu_a.
                     rewrite nth_error_replace_nth_eq in Hlookup_lhs.
                     +++ inversion Hlookup_lhs; subst lhs.
                         rewrite nth_error_replace_nth_neq in Hlookup_rhs by congruence.
                         apply (Hnodup cpu_idx cpu_b old_cpu rhs).
                         *** unfold kernel_cpu_slot. split; auto.
                         *** unfold kernel_cpu_slot. split; auto.
                         *** exact Hhas_lhs.
                         *** exact Hhas_rhs.
                         *** exact Htid.
                         *** exact Heq.
                     +++ destruct Hshape as [Hcount [_ Hcpus_len]].
                         rewrite Hcpus_len. lia.
                 --- subst cpu_b.
                     rewrite nth_error_replace_nth_neq in Hlookup_lhs by congruence.
                     rewrite nth_error_replace_nth_eq in Hlookup_rhs.
                     +++ inversion Hlookup_rhs; subst rhs.
                         apply (Hnodup cpu_a cpu_idx lhs old_cpu).
                         *** unfold kernel_cpu_slot. split; auto.
                         *** unfold kernel_cpu_slot. split; auto.
                         *** exact Hhas_lhs.
                         *** exact Hhas_rhs.
                         *** exact Htid.
                         *** exact Heq.
                     +++ destruct Hshape as [Hcount [_ Hcpus_len]].
                         rewrite Hcpus_len. lia.
                 --- rewrite nth_error_replace_nth_neq in Hlookup_lhs by congruence.
                     rewrite nth_error_replace_nth_neq in Hlookup_rhs by congruence.
                     apply (Hnodup cpu_a cpu_b lhs rhs);
                       unfold kernel_cpu_slot; auto.
              ** apply kernel_replace_cpu_preserves_activation_targets; auto.
Qed.

Theorem kernel_add_thread_invalid_cpu_preserves_invariant :
  forall sched cpu_idx thread_id generation weight slice_ns,
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = false ->
    kernel_sched_invariant
      (fst (kernel_add_thread sched cpu_idx thread_id generation weight slice_ns)).
Proof.
  intros sched cpu_idx thread_id generation weight slice_ns Hinv Hvalid.
  unfold kernel_add_thread.
  rewrite Hvalid.
  simpl.
  exact Hinv.
Qed.

Theorem kernel_add_thread_duplicate_preserves_invariant :
  forall sched cpu_idx owner_cpu thread_id generation weight slice_ns,
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_find_entity_cpu sched thread_id = Some owner_cpu ->
    kernel_sched_invariant
      (fst (kernel_add_thread sched cpu_idx thread_id generation weight slice_ns)).
Proof.
  intros sched cpu_idx owner_cpu thread_id generation weight slice_ns
    Hinv Hvalid Hfind.
  unfold kernel_add_thread.
  rewrite Hvalid.
  rewrite Hfind.
  simpl.
  exact Hinv.
Qed.

Theorem kernel_on_timer_invalid_cpu_preserves_invariant :
  forall sched cpu_idx runtime_ns,
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = false ->
    kernel_sched_invariant
      (fst (kernel_on_timer sched cpu_idx runtime_ns)).
Proof.
  intros sched cpu_idx runtime_ns Hinv Hvalid.
  unfold kernel_on_timer.
  rewrite Hvalid.
  simpl.
  exact Hinv.
Qed.

Theorem kernel_on_timer_no_current_preserves_invariant :
  forall sched cpu_idx runtime_ns,
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_cpu_current sched cpu_idx = None ->
    kernel_sched_invariant
      (fst (kernel_on_timer sched cpu_idx runtime_ns)).
Proof.
  intros sched cpu_idx runtime_ns Hinv Hvalid Hcurrent.
  unfold kernel_on_timer.
  rewrite Hvalid.
  rewrite Hcurrent.
  simpl.
  exact Hinv.
Qed.

Theorem kernel_on_timer_success_preserves_runqueues_invariant :
  forall sched cpu_idx runtime_ns thread_id generation rq current_index rq',
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_cpu_current sched cpu_idx = Some (thread_id, generation) ->
    kernel_lookup_runqueue sched cpu_idx = Some rq ->
    kernel_current_entity_index rq thread_id generation = Some current_index ->
    eevdf_charge rq thread_id runtime_ns = ok rq' ->
    kernel_runqueues_invariant
      (fst (kernel_on_timer sched cpu_idx runtime_ns)).
Proof.
  intros sched cpu_idx runtime_ns thread_id generation rq current_index rq'
    Hinv Hvalid Hcurrent Hlookup Hcurrent_entity Hcharge.
  destruct Hinv as
    [Hshape
      [Hrqs
      [_Hunique
      [_Hcurrent
      [_Hrunning
      [_Hnodup _Htargets]]]]]].
  assert (Hactive : kernel_active_runqueue sched cpu_idx rq).
  {
    unfold kernel_active_runqueue.
    split.
    - apply kernel_valid_cpu_lt.
      exact Hvalid.
    - unfold kernel_lookup_runqueue in Hlookup.
      exact Hlookup.
  }
  pose proof (Hrqs cpu_idx rq Hactive) as Hrq_inv.
  pose proof (charge_preserves_invariant
    rq
    thread_id
    runtime_ns
    rq'
    Hrq_inv
    Hcharge) as Hrq'_inv.
  unfold kernel_on_timer.
  rewrite Hvalid.
  rewrite Hcurrent.
  rewrite Hlookup.
  rewrite Hcurrent_entity.
  rewrite Hcharge.
  simpl.
  unfold kernel_runqueues_invariant.
  intros query_cpu local_rq Hactive_final.
  destruct Hactive_final as [Hlt_final Hlookup_final].
  unfold kernel_replace_runqueue in Hlookup_final.
  simpl in Hlookup_final.
  destruct (Nat.eq_dec query_cpu cpu_idx) as [Heq | Hneq].
  - subst query_cpu.
    rewrite nth_error_replace_nth_eq in Hlookup_final.
    + inversion Hlookup_final; subst local_rq.
      exact Hrq'_inv.
    + destruct Hshape as [Hcount [Hrqs_len _]].
      rewrite Hrqs_len.
      apply kernel_valid_cpu_lt in Hvalid.
      lia.
  - rewrite nth_error_replace_nth_neq in Hlookup_final by congruence.
    apply (Hrqs query_cpu local_rq).
    unfold kernel_active_runqueue.
    simpl in Hlt_final.
    split; auto.
Qed.

Theorem kernel_on_timer_success_preserves_current_matches :
  forall sched cpu_idx runtime_ns thread_id generation rq current_index rq',
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_cpu_current sched cpu_idx = Some (thread_id, generation) ->
    kernel_lookup_runqueue sched cpu_idx = Some rq ->
    kernel_current_entity_index rq thread_id generation = Some current_index ->
    eevdf_charge rq thread_id runtime_ns = ok rq' ->
    kernel_current_matches_local_running
      (fst (kernel_on_timer sched cpu_idx runtime_ns)).
Proof.
  intros sched cpu_idx runtime_ns thread_id generation rq current_index rq'
    Hinv Hvalid Hcurrent_value Hlookup Hcurrent_entity Hcharge.
  destruct Hinv as
    [Hshape
      [Hrqs
      [_Hunique
      [Hcurrent
      [_Hrunning
      [_Hnodup _Htargets]]]]]].
  assert (Hactive_target : kernel_active_runqueue sched cpu_idx rq).
  {
    unfold kernel_active_runqueue.
    split.
    - apply kernel_valid_cpu_lt.
      exact Hvalid.
    - unfold kernel_lookup_runqueue in Hlookup.
      exact Hlookup.
  }
  pose proof (Hrqs cpu_idx rq Hactive_target) as Hrq_inv.
  unfold kernel_on_timer.
  rewrite Hvalid.
  rewrite Hcurrent_value.
  rewrite Hlookup.
  rewrite Hcurrent_entity.
  rewrite Hcharge.
  simpl.
  unfold kernel_current_matches_local_running.
  intros query_cpu cpu Hslot_final Hhas.
  assert (Hslot_original : kernel_cpu_slot sched query_cpu cpu).
  {
    apply kernel_replace_runqueue_preserves_cpu_slot
      with (target_cpu := cpu_idx) (rq := rq').
    exact Hslot_final.
  }
  destruct (Hcurrent query_cpu cpu Hslot_original Hhas)
    as [old_rq [old_index [old_entity
      [Hold_active [Hold_entity [Hold_state [Hold_thread Hold_generation]]]]]]].
  destruct (Nat.eq_dec query_cpu cpu_idx) as [Heq | Hneq].
  - subst query_cpu.
    assert (Hold_rq_eq : old_rq = rq).
    {
      destruct Hold_active as [_ Hold_lookup].
      destruct Hactive_target as [_ Htarget_lookup].
      rewrite Hold_lookup in Htarget_lookup.
      inversion Htarget_lookup.
      reflexivity.
    }
    subst old_rq.
    destruct (kernel_cpu_current_slot_matches
      sched
      cpu_idx
      cpu
      thread_id
      generation
      Hcurrent_value
      Hslot_original) as [_Hcpu_has [Hcpu_thread Hcpu_generation]].
    assert (Hold_thread_id : ee_thread_id old_entity = thread_id).
    {
      rewrite Hold_thread.
      exact Hcpu_thread.
    }
    assert (Hold_generation_id : ee_generation old_entity = generation).
    {
      rewrite Hold_generation.
      exact Hcpu_generation.
    }
    destruct (eevdf_charge_success_has_matching_running_entity
      rq
      thread_id
      runtime_ns
      rq'
      old_index
      old_entity
      Hrq_inv
      Hold_entity
      Hold_state
      Hold_thread_id
      Hcharge) as [charged_index [charged_entity
        [Hcharged_active [Hcharged_state [Hcharged_thread Hcharged_generation]]]]].
    exists rq', charged_index, charged_entity.
    split.
    + unfold kernel_active_runqueue.
      split.
      * simpl.
        apply kernel_valid_cpu_lt.
        exact Hvalid.
      * unfold kernel_replace_runqueue.
        simpl.
        apply nth_error_replace_nth_eq.
        destruct Hshape as [Hcount [Hrqs_len _]].
        rewrite Hrqs_len.
        apply kernel_valid_cpu_lt in Hvalid.
        lia.
    + split.
      * exact Hcharged_active.
      * split.
        -- exact Hcharged_state.
        -- split.
           ++ rewrite Hcharged_thread.
              symmetry.
              exact Hcpu_thread.
           ++ rewrite Hcharged_generation.
              rewrite Hold_generation_id.
              symmetry.
              exact Hcpu_generation.
  - exists old_rq, old_index, old_entity.
    split.
    + destruct Hold_active as [Hold_lt Hold_lookup].
      unfold kernel_active_runqueue.
      split.
      * simpl.
        exact Hold_lt.
      * unfold kernel_replace_runqueue.
        simpl.
        rewrite nth_error_replace_nth_neq by congruence.
        exact Hold_lookup.
    + split.
      * exact Hold_entity.
      * repeat split; auto.
Qed.

Theorem kernel_on_timer_success_preserves_no_cross_cpu_current_duplicates :
  forall sched cpu_idx runtime_ns thread_id generation rq current_index rq',
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_cpu_current sched cpu_idx = Some (thread_id, generation) ->
    kernel_lookup_runqueue sched cpu_idx = Some rq ->
    kernel_current_entity_index rq thread_id generation = Some current_index ->
    eevdf_charge rq thread_id runtime_ns = ok rq' ->
    kernel_no_cross_cpu_current_duplicates
      (fst (kernel_on_timer sched cpu_idx runtime_ns)).
Proof.
  intros sched cpu_idx runtime_ns thread_id generation rq current_index rq'
    Hinv Hvalid Hcurrent Hlookup Hcurrent_entity Hcharge.
  destruct Hinv as
    [_Hshape
      [_Hrqs
      [_Hunique
      [_Hcurrent_matches
      [_Hrunning
      [Hnodup _Htargets]]]]]].
  unfold kernel_on_timer.
  rewrite Hvalid.
  rewrite Hcurrent.
  rewrite Hlookup.
  rewrite Hcurrent_entity.
  rewrite Hcharge.
  simpl.
  apply kernel_replace_runqueue_preserves_no_cross_cpu_current_duplicates.
  exact Hnodup.
Qed.

Theorem kernel_on_timer_success_preserves_activation_targets :
  forall sched cpu_idx runtime_ns thread_id generation rq current_index rq',
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_cpu_current sched cpu_idx = Some (thread_id, generation) ->
    kernel_lookup_runqueue sched cpu_idx = Some rq ->
    kernel_current_entity_index rq thread_id generation = Some current_index ->
    eevdf_charge rq thread_id runtime_ns = ok rq' ->
    kernel_activation_targets_valid_cpus
      (fst (kernel_on_timer sched cpu_idx runtime_ns)).
Proof.
  intros sched cpu_idx runtime_ns thread_id generation rq current_index rq'
    Hinv Hvalid Hcurrent Hlookup Hcurrent_entity Hcharge.
  destruct Hinv as
    [_Hshape
      [_Hrqs
      [_Hunique
      [_Hcurrent_matches
      [_Hrunning
      [_Hnodup Htargets]]]]]].
  unfold kernel_on_timer.
  rewrite Hvalid.
  rewrite Hcurrent.
  rewrite Hlookup.
  rewrite Hcurrent_entity.
  rewrite Hcharge.
  simpl.
  apply kernel_replace_runqueue_preserves_activation_targets.
  exact Htargets.
Qed.

Lemma eevdf_charge_active_entity_original :
  forall rq thread_id runtime_ns rq' index entity,
    eevdf_invariant rq ->
    eevdf_charge rq thread_id runtime_ns = ok rq' ->
    eevdf_active_entity rq' index entity ->
    is_active_state (ee_state entity) = true ->
    exists original,
      eevdf_active_entity rq index original /\
      is_active_state (ee_state original) = true /\
      ee_thread_id original = ee_thread_id entity /\
      ee_generation original = ee_generation entity /\
      ee_state original = ee_state entity.
Proof.
  intros rq thread_id runtime_ns rq' index entity
    Hinv Hcharge Hactive Hactive_state.
  destruct (eevdf_charge_success_formula rq thread_id runtime_ns rq' Hcharge)
    as [charged_index [old_entity [charged_entity [refreshed [delta Hsuccess]]]]].
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
  pose proof (find_entity_index_active
    rq
    thread_id
    charged_index
    old_entity
    Hfind
    Hlookup) as Hold_active.
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
  apply (proj1 (refresh_active_entity_iff _ _ _)) in Hactive.
  apply (proj1 (refresh_active_entity_iff _ _ _)) in Hactive.
  apply replace_entity_active_cases_shaped in Hactive.
  - destruct Hactive as [[Hindex Hentity] | [_Hneq Horiginal_active]].
    + subst index entity.
      exists old_entity.
      split.
      * exact Hold_active.
      * split.
        -- destruct Hold_active as [_ Hold_lookup].
           pose proof (find_entity_index_lookup_matches_thread
             rq
             thread_id
             charged_index
             old_entity
             Hfind
             Hold_lookup) as [_ Hold_active_state].
           exact Hold_active_state.
        -- split.
           ++ rewrite Hrefreshed_thread.
              rewrite Hcharged_thread.
              reflexivity.
	           ++ split.
	              ** rewrite Hrefreshed_generation.
	                 symmetry.
	                 exact Hcharged_generation.
	              ** rewrite Hrefreshed_state.
	                 symmetry.
	                 exact Hcharged_state.
    + exists entity.
      split.
      * exact Horiginal_active.
      * split.
        -- exact Hactive_state.
        -- split.
           ++ reflexivity.
           ++ split; reflexivity.
  - destruct Hinv as [Hshape _].
    exact Hshape.
Qed.

Lemma kernel_on_timer_success_entity_on_cpu_original :
  forall sched cpu_idx runtime_ns thread_id generation rq current_index rq'
      query_cpu index entity,
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_cpu_current sched cpu_idx = Some (thread_id, generation) ->
    kernel_lookup_runqueue sched cpu_idx = Some rq ->
    kernel_current_entity_index rq thread_id generation = Some current_index ->
    eevdf_charge rq thread_id runtime_ns = ok rq' ->
    kernel_entity_on_cpu
      (kernel_replace_runqueue sched cpu_idx rq')
      query_cpu
      index
      entity ->
    exists original,
      kernel_entity_on_cpu sched query_cpu index original /\
      ee_thread_id original = ee_thread_id entity /\
      ee_generation original = ee_generation entity /\
      ee_state original = ee_state entity.
Proof.
  intros sched cpu_idx runtime_ns thread_id generation rq current_index rq'
    query_cpu index entity Hinv Hvalid _Hcurrent Hlookup _Hcurrent_entity
    Hcharge Hentity.
  destruct Hinv as
    [Hshape
      [Hrqs
      [_Hunique
      [_Hcurrent_matches
      [_Hrunning
      [_Hnodup _Htargets]]]]]].
  assert (Htarget_active : kernel_active_runqueue sched cpu_idx rq).
  {
    unfold kernel_active_runqueue.
    split.
    - apply kernel_valid_cpu_lt.
      exact Hvalid.
    - unfold kernel_lookup_runqueue in Hlookup.
      exact Hlookup.
  }
  pose proof (Hrqs cpu_idx rq Htarget_active) as Hrq_inv.
  destruct Hentity as [local_rq [Hactive_rq [Hactive_entity Hactive_state]]].
  destruct (Nat.eq_dec query_cpu cpu_idx) as [Heq | Hneq].
  - subst query_cpu.
    destruct Hactive_rq as [Hlt_final Hlookup_final].
    unfold kernel_replace_runqueue in Hlookup_final.
    simpl in Hlookup_final.
    rewrite nth_error_replace_nth_eq in Hlookup_final.
    + inversion Hlookup_final; subst local_rq.
      destruct (eevdf_charge_active_entity_original
        rq
        thread_id
        runtime_ns
        rq'
        index
        entity
        Hrq_inv
        Hcharge
        Hactive_entity
        Hactive_state) as
        [original
          [Horiginal_active
          [Horiginal_active_state
          [Horiginal_thread
          [Horiginal_generation Horiginal_state]]]]].
      exists original.
      split.
      * unfold kernel_entity_on_cpu.
        exists rq.
        split.
        -- exact Htarget_active.
        -- split.
           ++ exact Horiginal_active.
           ++ exact Horiginal_active_state.
      * repeat split; auto.
    + destruct Hshape as [Hcount [Hrqs_len _]].
      rewrite Hrqs_len.
      apply kernel_valid_cpu_lt in Hvalid.
      lia.
  - exists entity.
    split.
    + unfold kernel_entity_on_cpu.
      exists local_rq.
      split.
      * destruct Hactive_rq as [Hlt_final Hlookup_final].
        unfold kernel_active_runqueue.
        split.
        -- simpl in Hlt_final.
           exact Hlt_final.
        -- unfold kernel_replace_runqueue in Hlookup_final.
           simpl in Hlookup_final.
           rewrite nth_error_replace_nth_neq in Hlookup_final by congruence.
           exact Hlookup_final.
      * split; auto.
    + repeat split; reflexivity.
Qed.

Theorem kernel_on_timer_success_preserves_global_thread_ids_unique :
  forall sched cpu_idx runtime_ns thread_id generation rq current_index rq',
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_cpu_current sched cpu_idx = Some (thread_id, generation) ->
    kernel_lookup_runqueue sched cpu_idx = Some rq ->
    kernel_current_entity_index rq thread_id generation = Some current_index ->
    eevdf_charge rq thread_id runtime_ns = ok rq' ->
    kernel_global_thread_ids_unique
      (fst (kernel_on_timer sched cpu_idx runtime_ns)).
Proof.
  intros sched cpu_idx runtime_ns thread_id generation rq current_index rq'
    Hinv Hvalid Hcurrent Hlookup Hcurrent_entity Hcharge.
  destruct Hinv as
    [Hshape
      [Hrqs
      [Hunique
      [Hcurrent_matches
      [Hrunning
      [Hnodup Htargets]]]]]].
  pose proof
    (conj Hshape
      (conj Hrqs
      (conj Hunique
      (conj Hcurrent_matches
      (conj Hrunning
      (conj Hnodup Htargets)))))) as Hinv_full.
  unfold kernel_on_timer.
  rewrite Hvalid.
  rewrite Hcurrent.
  rewrite Hlookup.
  rewrite Hcurrent_entity.
  rewrite Hcharge.
  simpl.
  unfold kernel_global_thread_ids_unique.
  intros cpu_a index_a cpu_b index_b lhs rhs Hlhs Hrhs Hthread Heq.
  destruct (kernel_on_timer_success_entity_on_cpu_original
    sched cpu_idx runtime_ns thread_id generation rq current_index rq'
    cpu_a index_a lhs
    Hinv_full Hvalid Hcurrent Hlookup Hcurrent_entity Hcharge Hlhs) as
    [orig_lhs [Horig_lhs [Horig_lhs_thread [_Horig_lhs_gen _Horig_lhs_state]]]].
  destruct (kernel_on_timer_success_entity_on_cpu_original
    sched cpu_idx runtime_ns thread_id generation rq current_index rq'
    cpu_b index_b rhs
    Hinv_full Hvalid Hcurrent Hlookup Hcurrent_entity Hcharge Hrhs) as
    [orig_rhs [Horig_rhs [Horig_rhs_thread [_Horig_rhs_gen _Horig_rhs_state]]]].
  apply Hunique with (lhs := orig_lhs) (rhs := orig_rhs); auto.
  - rewrite Horig_lhs_thread.
    exact Hthread.
  - rewrite Horig_lhs_thread.
    rewrite Horig_rhs_thread.
    exact Heq.
Qed.

Theorem kernel_on_timer_success_preserves_running_entity_has_current :
  forall sched cpu_idx runtime_ns thread_id generation rq current_index rq',
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_cpu_current sched cpu_idx = Some (thread_id, generation) ->
    kernel_lookup_runqueue sched cpu_idx = Some rq ->
    kernel_current_entity_index rq thread_id generation = Some current_index ->
    eevdf_charge rq thread_id runtime_ns = ok rq' ->
    kernel_running_entity_has_current
      (fst (kernel_on_timer sched cpu_idx runtime_ns)).
Proof.
  intros sched cpu_idx runtime_ns thread_id generation rq current_index rq'
    Hinv Hvalid Hcurrent Hlookup Hcurrent_entity Hcharge.
  destruct Hinv as
    [Hshape
      [Hrqs
      [Hunique
      [Hcurrent_matches
      [Hrunning
      [Hnodup Htargets]]]]]].
  pose proof
    (conj Hshape
      (conj Hrqs
      (conj Hunique
      (conj Hcurrent_matches
      (conj Hrunning
      (conj Hnodup Htargets)))))) as Hinv_full.
  unfold kernel_on_timer.
  rewrite Hvalid.
  rewrite Hcurrent.
  rewrite Hlookup.
  rewrite Hcurrent_entity.
  rewrite Hcharge.
  simpl.
  unfold kernel_running_entity_has_current.
  intros run_cpu local_rq index entity Hactive_final Hentity_active Hstate.
  assert (Hentity_final :
    kernel_entity_on_cpu
      (kernel_replace_runqueue sched cpu_idx rq')
      run_cpu
      index
      entity).
  {
    unfold kernel_entity_on_cpu.
    exists local_rq.
    split.
    - exact Hactive_final.
    - split.
      + exact Hentity_active.
      + rewrite Hstate.
        reflexivity.
  }
  destruct (kernel_on_timer_success_entity_on_cpu_original
    sched cpu_idx runtime_ns thread_id generation rq current_index rq'
    run_cpu index entity
    Hinv_full Hvalid Hcurrent Hlookup Hcurrent_entity Hcharge Hentity_final) as
    [original [Horiginal_entity
      [Horiginal_thread [Horiginal_generation Horiginal_state]]]].
  destruct Horiginal_entity as
    [original_rq [Horiginal_rq [Horiginal_active _Horiginal_active_state]]].
  destruct (Hrunning run_cpu original_rq index original
    Horiginal_rq
    Horiginal_active
    (eq_trans Horiginal_state Hstate)) as
    [cpu [Hslot [Hhas [Htid Hgen]]]].
  exists cpu.
  split.
  - apply kernel_replace_runqueue_preserves_cpu_slot_forward.
    exact Hslot.
  - split.
    + exact Hhas.
    + split.
      * rewrite <- Horiginal_thread.
        exact Htid.
      * rewrite <- Horiginal_generation.
        exact Hgen.
Qed.

Theorem kernel_on_timer_success_preserves_invariant :
  forall sched cpu_idx runtime_ns thread_id generation rq current_index rq',
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_cpu_current sched cpu_idx = Some (thread_id, generation) ->
    kernel_lookup_runqueue sched cpu_idx = Some rq ->
    kernel_current_entity_index rq thread_id generation = Some current_index ->
    eevdf_charge rq thread_id runtime_ns = ok rq' ->
    kernel_sched_invariant
      (fst (kernel_on_timer sched cpu_idx runtime_ns)).
Proof.
  intros sched cpu_idx runtime_ns thread_id generation rq current_index rq'
    Hinv Hvalid Hcurrent Hlookup Hcurrent_entity Hcharge.
  destruct Hinv as
    [Hshape
      [Hrqs
      [Hunique
      [Hcurrent_matches
      [Hrunning
      [Hnodup Htargets]]]]]].
  pose proof
    (conj Hshape
      (conj Hrqs
      (conj Hunique
      (conj Hcurrent_matches
      (conj Hrunning
      (conj Hnodup Htargets)))))) as Hinv_full.
  split.
  - unfold kernel_on_timer.
    rewrite Hvalid.
    rewrite Hcurrent.
    rewrite Hlookup.
    rewrite Hcurrent_entity.
    rewrite Hcharge.
    simpl.
    apply kernel_replace_runqueue_shape.
    exact Hshape.
  - split.
    + apply kernel_on_timer_success_preserves_runqueues_invariant
        with
          (runtime_ns := runtime_ns)
          (rq := rq)
          (current_index := current_index)
          (rq' := rq')
          (thread_id := thread_id)
          (generation := generation); auto.
    + split.
      * apply kernel_on_timer_success_preserves_global_thread_ids_unique
          with
            (runtime_ns := runtime_ns)
            (rq := rq)
            (current_index := current_index)
            (rq' := rq')
            (thread_id := thread_id)
            (generation := generation); auto.
      * split.
        -- apply kernel_on_timer_success_preserves_current_matches
             with
               (runtime_ns := runtime_ns)
               (rq := rq)
               (current_index := current_index)
               (rq' := rq')
               (thread_id := thread_id)
               (generation := generation); auto.
        -- split.
           ++ apply kernel_on_timer_success_preserves_running_entity_has_current
                with
                  (runtime_ns := runtime_ns)
                  (rq := rq)
                  (current_index := current_index)
                  (rq' := rq')
                  (thread_id := thread_id)
                  (generation := generation); auto.
           ++ split.
              ** apply kernel_on_timer_success_preserves_no_cross_cpu_current_duplicates
                   with
                     (runtime_ns := runtime_ns)
                     (rq := rq)
                     (current_index := current_index)
                     (rq' := rq')
                     (thread_id := thread_id)
                     (generation := generation); auto.
              ** apply kernel_on_timer_success_preserves_activation_targets
                   with
                     (runtime_ns := runtime_ns)
                     (rq := rq)
                     (current_index := current_index)
                     (rq' := rq')
                     (thread_id := thread_id)
                     (generation := generation); auto.
Qed.

Theorem kernel_pick_invalid_cpu_preserves_invariant :
  forall sched cpu_idx,
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = false ->
    kernel_sched_invariant
      (fst (kernel_pick_cpu sched cpu_idx)).
Proof.
  intros sched cpu_idx Hinv Hvalid.
  unfold kernel_pick_cpu.
  rewrite Hvalid.
  simpl.
  exact Hinv.
Qed.

Theorem kernel_pick_busy_cpu_preserves_invariant :
  forall sched cpu_idx,
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_cpu_has_current sched cpu_idx = true ->
    kernel_sched_invariant
      (fst (kernel_pick_cpu sched cpu_idx)).
Proof.
  intros sched cpu_idx Hinv Hvalid Hcurrent.
  unfold kernel_pick_cpu.
  rewrite Hvalid.
  rewrite Hcurrent.
  simpl.
  exact Hinv.
Qed.

Theorem kernel_pick_run_thread_success_preserves_invariant :
  forall sched cpu_idx rq rq_after_pick picked_index picked_entity rq',
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_cpu_has_current sched cpu_idx = false ->
    kernel_lookup_runqueue sched cpu_idx = Some rq ->
    eevdf_pick rq = (rq_after_pick, Some (picked_index, picked_entity)) ->
    eevdf_mark_running rq_after_pick (ee_thread_id picked_entity) = ok rq' ->
    kernel_sched_invariant
      (fst (kernel_pick_cpu sched cpu_idx)).
Proof.
  intros sched cpu_idx rq rq_after_pick picked_index picked_entity rq'
    Hinv Hvalid Hcpu_idle Hlookup Hpick Hmark.
  pose proof Hinv as Hinv_full.
  destruct Hinv as
    [Hshape
      [Hrqs
      [Hunique
      [Hcurrent
      [Hrunning
      [Hnodup Htargets]]]]]].
  unfold kernel_pick_cpu.
  rewrite Hvalid.
  rewrite Hcpu_idle.
  rewrite Hlookup.
  rewrite Hpick.
  rewrite Hmark.
  simpl.
  split.
  - apply kernel_pick_run_thread_success_preserves_shape.
    exact Hshape.
  - split.
    + apply kernel_pick_run_thread_success_preserves_runqueues_invariant
        with
          (rq := rq)
          (picked_index := picked_index); auto.
    + split.
      * apply kernel_pick_run_thread_success_preserves_global_thread_ids_unique
          with
            (rq := rq)
            (rq_after_pick := rq_after_pick)
            (picked_index := picked_index); auto.
      * split.
        -- apply kernel_pick_run_thread_success_preserves_current_matches
             with
               (rq := rq)
               (rq_after_pick := rq_after_pick)
               (picked_index := picked_index); auto.
        -- split.
           ++ apply kernel_pick_run_thread_success_preserves_running_entity_has_current
                with
                  (rq := rq)
                  (rq_after_pick := rq_after_pick)
                  (picked_index := picked_index); auto.
           ++ split.
              ** apply kernel_pick_run_thread_success_preserves_no_cross_cpu_current_duplicates
                   with
                     (rq := rq)
                     (rq_after_pick := rq_after_pick)
                     (picked_index := picked_index); auto.
              ** apply kernel_pick_run_thread_success_preserves_activation_targets; auto.
Qed.

Theorem kernel_pick_idle_success_preserves_invariant :
  forall sched cpu_idx rq rq_after_pick,
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_cpu_has_current sched cpu_idx = false ->
    kernel_lookup_runqueue sched cpu_idx = Some rq ->
    eevdf_pick rq = (rq_after_pick, None) ->
    kernel_sched_invariant
      (fst (kernel_pick_cpu sched cpu_idx)).
Proof.
  intros sched cpu_idx rq rq_after_pick Hinv Hvalid Hcurrent Hlookup Hpick.
  destruct Hinv as
    [Hshape
      [Hrqs
      [Hunique
      [Hcurrent_matches
      [Hrunning
      [Hnodup Htargets]]]]]].
  assert (Hactive : kernel_active_runqueue sched cpu_idx rq).
  {
    unfold kernel_active_runqueue.
    split.
    - apply kernel_valid_cpu_lt. exact Hvalid.
    - unfold kernel_lookup_runqueue in Hlookup. exact Hlookup.
  }
  pose proof (Hrqs cpu_idx rq Hactive) as Hrq_inv.
  pose proof (eevdf_pick_none_returns_same_runqueue
    rq
    rq_after_pick
    Hrq_inv
    Hpick) as Hsame.
  subst rq_after_pick.
  unfold kernel_pick_cpu.
  rewrite Hvalid.
  rewrite Hcurrent.
  rewrite Hlookup.
  rewrite Hpick.
  simpl.
  rewrite kernel_replace_runqueue_same with (rq := rq); auto.
  apply kernel_set_activation_pending_preserves_invariant.
  - exact
      (conj Hshape
        (conj Hrqs
        (conj Hunique
        (conj Hcurrent_matches
        (conj Hrunning
        (conj Hnodup Htargets)))))).
  - exact Hvalid.
Qed.

Theorem kernel_finish_current_invalid_cpu_preserves_invariant :
  forall sched cpu_idx,
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = false ->
    kernel_sched_invariant
      (fst (kernel_finish_current sched cpu_idx)).
Proof.
  intros sched cpu_idx Hinv Hvalid.
  unfold kernel_finish_current.
  rewrite Hvalid.
  simpl.
  exact Hinv.
Qed.

Theorem kernel_finish_current_no_current_preserves_invariant :
  forall sched cpu_idx,
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_cpu_current sched cpu_idx = None ->
    kernel_sched_invariant
      (fst (kernel_finish_current sched cpu_idx)).
Proof.
  intros sched cpu_idx Hinv Hvalid Hcurrent.
  unfold kernel_finish_current.
  rewrite Hvalid.
  rewrite Hcurrent.
  simpl.
  exact Hinv.
Qed.

Theorem kernel_finish_current_success_preserves_runqueues_invariant :
  forall sched cpu_idx thread_id generation rq current_index rq',
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_cpu_current sched cpu_idx = Some (thread_id, generation) ->
    kernel_lookup_runqueue sched cpu_idx = Some rq ->
    kernel_current_entity_index rq thread_id generation = Some current_index ->
    eevdf_requeue_running rq thread_id = ok rq' ->
    kernel_runqueues_invariant
      (fst (kernel_finish_current sched cpu_idx)).
Proof.
  intros sched cpu_idx thread_id generation rq current_index rq'
    Hinv Hvalid Hcurrent Hlookup Hcurrent_entity Hrequeue.
  destruct Hinv as
    [Hshape
      [Hrqs
      [_Hunique
      [_Hcurrent
      [_Hrunning
      [_Hnodup _Htargets]]]]]].
  assert (Hactive : kernel_active_runqueue sched cpu_idx rq).
  {
    unfold kernel_active_runqueue.
    split.
    - apply kernel_valid_cpu_lt.
      exact Hvalid.
    - unfold kernel_lookup_runqueue in Hlookup.
      exact Hlookup.
  }
  pose proof (Hrqs cpu_idx rq Hactive) as Hrq_inv.
  pose proof (requeue_running_preserves_invariant
    rq
    thread_id
    rq'
    Hrq_inv
    Hrequeue) as Hrq'_inv.
  unfold kernel_finish_current.
  rewrite Hvalid.
  rewrite Hcurrent.
  rewrite Hlookup.
  rewrite Hcurrent_entity.
  rewrite Hrequeue.
  simpl.
  unfold kernel_runqueues_invariant.
  intros query_cpu local_rq Hactive_final.
  destruct Hactive_final as [Hlt_final Hlookup_final].
  rewrite kernel_clear_cpu_current_preserves_runqueues in Hlookup_final.
  unfold kernel_replace_runqueue in Hlookup_final.
  simpl in Hlookup_final.
  destruct (Nat.eq_dec query_cpu cpu_idx) as [Heq | Hneq].
  - subst query_cpu.
    rewrite nth_error_replace_nth_eq in Hlookup_final.
    + inversion Hlookup_final; subst local_rq.
      exact Hrq'_inv.
    + destruct Hshape as [Hcount [Hrqs_len _]].
      rewrite Hrqs_len.
      apply kernel_valid_cpu_lt in Hvalid.
      lia.
  - rewrite nth_error_replace_nth_neq in Hlookup_final by congruence.
    apply (Hrqs query_cpu local_rq).
    unfold kernel_active_runqueue.
    rewrite kernel_clear_cpu_current_preserves_cpu_count in Hlt_final.
    simpl in Hlt_final.
    split; auto.
Qed.

Theorem kernel_finish_current_success_preserves_current_matches :
  forall sched cpu_idx thread_id generation rq current_index rq',
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_cpu_current sched cpu_idx = Some (thread_id, generation) ->
    kernel_lookup_runqueue sched cpu_idx = Some rq ->
    kernel_current_entity_index rq thread_id generation = Some current_index ->
    eevdf_requeue_running rq thread_id = ok rq' ->
    kernel_current_matches_local_running
      (fst (kernel_finish_current sched cpu_idx)).
Proof.
  intros sched cpu_idx thread_id generation rq current_index rq'
    Hinv Hvalid Hcurrent_value Hlookup Hcurrent_entity Hrequeue.
  destruct Hinv as
    [Hshape
      [_Hrqs
      [_Hunique
      [Hcurrent
      [_Hrunning
      [_Hnodup _Htargets]]]]]].
  unfold kernel_finish_current.
  rewrite Hvalid.
  rewrite Hcurrent_value.
  rewrite Hlookup.
  rewrite Hcurrent_entity.
  rewrite Hrequeue.
  simpl.
  unfold kernel_current_matches_local_running.
  intros query_cpu cpu Hslot_final Hhas.
  destruct (Nat.eq_dec query_cpu cpu_idx) as [Heq | Hneq].
  - subst query_cpu.
    unfold kernel_clear_cpu_current in Hslot_final.
    assert (Hlookup_cpu :
      exists old_cpu,
        kernel_lookup_cpu (kernel_replace_runqueue sched cpu_idx rq') cpu_idx =
        Some old_cpu).
    {
      apply kernel_lookup_cpu_some.
      - apply kernel_replace_runqueue_shape.
        exact Hshape.
      - unfold kernel_valid_cpu.
        rewrite kernel_replace_runqueue_preserves_cpu_count.
        exact Hvalid.
    }
    destruct Hlookup_cpu as [old_cpu Hlookup_cpu].
    rewrite Hlookup_cpu in Hslot_final.
    unfold kernel_cpu_slot, kernel_replace_cpu in Hslot_final.
    simpl in Hslot_final.
    destruct Hslot_final as [_ Hslot_lookup].
    rewrite nth_error_replace_nth_eq in Hslot_lookup.
    + inversion Hslot_lookup; subst cpu.
      simpl in Hhas.
      discriminate.
    + destruct Hshape as [Hcount [_ Hcpus_len]].
      rewrite Hcpus_len.
      apply kernel_valid_cpu_lt in Hvalid.
      lia.
  - assert (Hslot_original : kernel_cpu_slot sched query_cpu cpu).
    {
      unfold kernel_clear_cpu_current in Hslot_final.
      destruct (kernel_lookup_cpu (kernel_replace_runqueue sched cpu_idx rq') cpu_idx)
        as [old_cpu |] eqn:Hlookup_cpu.
      - apply kernel_replace_runqueue_preserves_cpu_slot
          with (target_cpu := cpu_idx) (rq := rq').
        destruct Hslot_final as [Hlt Hslot_lookup].
        unfold kernel_cpu_slot, kernel_replace_cpu.
        simpl in *.
        split; auto.
        rewrite nth_error_replace_nth_neq in Hslot_lookup by congruence.
        exact Hslot_lookup.
      - apply kernel_replace_runqueue_preserves_cpu_slot
          with (target_cpu := cpu_idx) (rq := rq').
        exact Hslot_final.
    }
    destruct (Hcurrent query_cpu cpu Hslot_original Hhas)
      as [old_rq [old_index [old_entity
        [Hold_active [Hold_entity [Hold_state [Hold_thread Hold_generation]]]]]]].
    exists old_rq, old_index, old_entity.
    split.
    + destruct Hold_active as [Hold_lt Hold_lookup].
      unfold kernel_active_runqueue.
      rewrite kernel_clear_cpu_current_preserves_cpu_count.
      simpl.
      split.
      * exact Hold_lt.
      * rewrite kernel_clear_cpu_current_preserves_runqueues.
        unfold kernel_replace_runqueue.
        simpl.
	        rewrite nth_error_replace_nth_neq by congruence.
	        exact Hold_lookup.
    + split.
      * exact Hold_entity.
      * split.
        -- exact Hold_state.
        -- split; auto.
Qed.

Theorem kernel_finish_current_success_preserves_no_cross_cpu_current_duplicates :
  forall sched cpu_idx thread_id generation rq current_index rq',
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_cpu_current sched cpu_idx = Some (thread_id, generation) ->
    kernel_lookup_runqueue sched cpu_idx = Some rq ->
    kernel_current_entity_index rq thread_id generation = Some current_index ->
    eevdf_requeue_running rq thread_id = ok rq' ->
    kernel_no_cross_cpu_current_duplicates
      (fst (kernel_finish_current sched cpu_idx)).
Proof.
  intros sched cpu_idx thread_id generation rq current_index rq'
    Hinv Hvalid Hcurrent Hlookup Hcurrent_entity Hrequeue.
  destruct Hinv as
    [_Hshape
      [_Hrqs
      [_Hunique
      [_Hcurrent_matches
      [_Hrunning
      [Hnodup _Htargets]]]]]].
  unfold kernel_finish_current.
  rewrite Hvalid.
  rewrite Hcurrent.
  rewrite Hlookup.
  rewrite Hcurrent_entity.
  rewrite Hrequeue.
  simpl.
  apply kernel_clear_cpu_current_preserves_no_cross_cpu_current_duplicates.
  apply kernel_replace_runqueue_preserves_no_cross_cpu_current_duplicates.
  exact Hnodup.
Qed.

Theorem kernel_finish_current_success_preserves_activation_targets :
  forall sched cpu_idx thread_id generation rq current_index rq',
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_cpu_current sched cpu_idx = Some (thread_id, generation) ->
    kernel_lookup_runqueue sched cpu_idx = Some rq ->
    kernel_current_entity_index rq thread_id generation = Some current_index ->
    eevdf_requeue_running rq thread_id = ok rq' ->
    kernel_activation_targets_valid_cpus
      (fst (kernel_finish_current sched cpu_idx)).
Proof.
  intros sched cpu_idx thread_id generation rq current_index rq'
    Hinv Hvalid Hcurrent Hlookup Hcurrent_entity Hrequeue.
  destruct Hinv as
    [Hshape
      [_Hrqs
      [_Hunique
      [_Hcurrent_matches
      [_Hrunning
      [_Hnodup Htargets]]]]]].
  unfold kernel_finish_current.
  rewrite Hvalid.
  rewrite Hcurrent.
  rewrite Hlookup.
  rewrite Hcurrent_entity.
  rewrite Hrequeue.
  simpl.
  apply kernel_clear_cpu_current_preserves_activation_targets.
  - apply kernel_replace_runqueue_shape.
    exact Hshape.
  - unfold kernel_valid_cpu.
    rewrite kernel_replace_runqueue_preserves_cpu_count.
    exact Hvalid.
  - apply kernel_replace_runqueue_preserves_activation_targets.
    exact Htargets.
Qed.

Lemma eevdf_requeue_active_entity_original :
  forall rq thread_id rq' index entity,
    eevdf_invariant rq ->
    eevdf_requeue_running rq thread_id = ok rq' ->
    eevdf_active_entity rq' index entity ->
    is_active_state (ee_state entity) = true ->
    exists original,
      eevdf_active_entity rq index original /\
      is_active_state (ee_state original) = true /\
      ee_thread_id original = ee_thread_id entity /\
      ee_generation original = ee_generation entity.
Proof.
  intros rq thread_id rq' index entity
    Hinv Hrequeue Hactive Hactive_state.
  destruct (eevdf_requeue_running_success_formula rq thread_id rq' Hrequeue)
    as [requeued_index [source [refreshed
      [Hfind [Hlookup [Hsource_state [Hrefresh Hrq']]]]]]].
  pose proof (find_entity_index_active
    rq
    thread_id
    requeued_index
    source
    Hfind
    Hlookup) as Hsource_active.
  pose proof (refresh_deadline_success
    (set_entity_state source ERunnable)
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
  apply (proj1 (refresh_active_entity_iff _ _ _)) in Hactive.
  apply replace_entity_active_cases_shaped in Hactive.
  - destruct Hactive as [[Hindex Hentity] | [_Hneq Horiginal_active]].
    + subst index entity.
      exists source.
      split.
      * exact Hsource_active.
      * split.
        -- rewrite Hsource_state.
           reflexivity.
        -- split.
           ++ rewrite Hrefreshed_thread.
              reflexivity.
           ++ rewrite Hrefreshed_generation.
              reflexivity.
    + exists entity.
      split.
      * exact Horiginal_active.
      * split.
        -- exact Hactive_state.
        -- split; reflexivity.
  - destruct Hinv as [Hshape _].
    exact Hshape.
Qed.

Lemma eevdf_requeue_running_entity_original_not_target :
  forall rq thread_id rq' index entity,
    eevdf_invariant rq ->
    eevdf_requeue_running rq thread_id = ok rq' ->
    eevdf_active_entity rq' index entity ->
    ee_state entity = ERunning ->
    exists original,
      eevdf_active_entity rq index original /\
      ee_state original = ERunning /\
      ee_thread_id original = ee_thread_id entity /\
      ee_generation original = ee_generation entity /\
      ee_thread_id original <> thread_id.
Proof.
  intros rq thread_id rq' index entity
    Hinv Hrequeue Hactive Hrunning.
  destruct (eevdf_requeue_running_success_formula rq thread_id rq' Hrequeue)
    as [requeued_index [source [refreshed
      [Hfind [Hlookup [Hsource_state [Hrefresh Hrq']]]]]]].
  pose proof (find_entity_index_active
    rq
    thread_id
    requeued_index
    source
    Hfind
    Hlookup) as Hsource_active.
  pose proof (find_entity_index_lookup_matches_thread
    rq
    thread_id
    requeued_index
    source
    Hfind
    Hlookup) as [Hsource_thread _Hsource_active_state].
  assert (Hthread_not_none : thread_id <> no_thread_id).
  {
    unfold find_entity_index in Hfind.
    destruct (thread_id =? no_thread_id) eqn:Hno; try discriminate.
    apply Z.eqb_neq in Hno.
    exact Hno.
  }
  pose proof (refresh_deadline_success
    (set_entity_state source ERunnable)
    (er_min_vruntime rq)
    refreshed
    Hrefresh) as
    [_Hrefreshed_thread
    [_Hrefreshed_generation
    [_Hrefreshed_weight
    [_Hrefreshed_slice
    [_Hrefreshed_service
    [_Hrefreshed_vruntime
    [Hrefreshed_state _Hrefreshed_deadline]]]]]]].
  subst rq'.
  apply (proj1 (refresh_active_entity_iff _ _ _)) in Hactive.
  apply replace_entity_active_cases_shaped in Hactive.
  - destruct Hactive as [[Hindex Hentity] | [Hneq Horiginal_active]].
    + subst index entity.
      rewrite Hrefreshed_state in Hrunning.
      simpl in Hrunning.
      discriminate.
    + exists entity.
      split.
      * exact Horiginal_active.
      * split.
        -- exact Hrunning.
        -- split.
           ++ reflexivity.
           ++ split.
              ** reflexivity.
              ** intros Hsame_thread.
                 destruct Hinv as
                   [_Hshape
                   [_Hinactive
                   [Hunique
                   [_Hpositive
                   [_Hlive
                   [_Hmin
                   [_Hvirtual _Hrunnable]]]]]]].
                 assert (Hsame_index : index = requeued_index).
                 {
                   apply Hunique with (lhs := entity) (rhs := source).
                   - exact Horiginal_active.
                   - exact Hsource_active.
	                   - rewrite Hsame_thread.
	                     exact Hthread_not_none.
	                   - rewrite Hsame_thread.
	                     symmetry.
	                     exact Hsource_thread.
                   - rewrite Hrunning.
                     reflexivity.
                   - rewrite Hsource_state.
                     reflexivity.
                 }
                 contradiction.
  - destruct Hinv as [Hshape _].
    exact Hshape.
Qed.

Lemma kernel_finish_current_success_entity_on_cpu_original :
  forall sched cpu_idx thread_id generation rq current_index rq'
      query_cpu index entity,
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_cpu_current sched cpu_idx = Some (thread_id, generation) ->
    kernel_lookup_runqueue sched cpu_idx = Some rq ->
    kernel_current_entity_index rq thread_id generation = Some current_index ->
    eevdf_requeue_running rq thread_id = ok rq' ->
    kernel_entity_on_cpu
      (kernel_clear_cpu_current
        (kernel_replace_runqueue sched cpu_idx rq')
        cpu_idx)
      query_cpu
      index
      entity ->
    exists original,
      kernel_entity_on_cpu sched query_cpu index original /\
      ee_thread_id original = ee_thread_id entity /\
      ee_generation original = ee_generation entity.
Proof.
  intros sched cpu_idx thread_id generation rq current_index rq'
    query_cpu index entity Hinv Hvalid _Hcurrent Hlookup _Hcurrent_entity
    Hrequeue Hentity.
  destruct Hinv as
    [Hshape
      [Hrqs
      [_Hunique
      [_Hcurrent_matches
      [_Hrunning
      [_Hnodup _Htargets]]]]]].
  assert (Htarget_active : kernel_active_runqueue sched cpu_idx rq).
  {
    unfold kernel_active_runqueue.
    split.
    - apply kernel_valid_cpu_lt.
      exact Hvalid.
    - unfold kernel_lookup_runqueue in Hlookup.
      exact Hlookup.
  }
  pose proof (Hrqs cpu_idx rq Htarget_active) as Hrq_inv.
  destruct Hentity as [local_rq [Hactive_rq [Hactive_entity Hactive_state]]].
  destruct (Nat.eq_dec query_cpu cpu_idx) as [Heq | Hneq].
  - subst query_cpu.
    destruct Hactive_rq as [Hlt_final Hlookup_final].
    rewrite kernel_clear_cpu_current_preserves_runqueues in Hlookup_final.
    unfold kernel_replace_runqueue in Hlookup_final.
    simpl in Hlookup_final.
    rewrite nth_error_replace_nth_eq in Hlookup_final.
    + inversion Hlookup_final; subst local_rq.
      destruct (eevdf_requeue_active_entity_original
        rq
        thread_id
        rq'
        index
        entity
        Hrq_inv
        Hrequeue
        Hactive_entity
        Hactive_state) as
        [original
          [Horiginal_active
          [_Horiginal_active_state
          [Horiginal_thread Horiginal_generation]]]].
      exists original.
      split.
      * unfold kernel_entity_on_cpu.
        exists rq.
        split.
	        -- exact Htarget_active.
	        -- split.
	           ++ exact Horiginal_active.
	           ++ exact _Horiginal_active_state.
      * split; auto.
    + destruct Hshape as [Hcount [Hrqs_len _]].
      rewrite Hrqs_len.
      apply kernel_valid_cpu_lt in Hvalid.
      lia.
  - exists entity.
    split.
    + unfold kernel_entity_on_cpu.
      exists local_rq.
      split.
      * destruct Hactive_rq as [Hlt_final Hlookup_final].
        unfold kernel_active_runqueue.
        split.
        -- rewrite kernel_clear_cpu_current_preserves_cpu_count in Hlt_final.
           simpl in Hlt_final.
           exact Hlt_final.
        -- rewrite kernel_clear_cpu_current_preserves_runqueues in Hlookup_final.
           unfold kernel_replace_runqueue in Hlookup_final.
           simpl in Hlookup_final.
           rewrite nth_error_replace_nth_neq in Hlookup_final by congruence.
           exact Hlookup_final.
      * split; auto.
    + split; reflexivity.
Qed.

Theorem kernel_finish_current_success_preserves_global_thread_ids_unique :
  forall sched cpu_idx thread_id generation rq current_index rq',
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_cpu_current sched cpu_idx = Some (thread_id, generation) ->
    kernel_lookup_runqueue sched cpu_idx = Some rq ->
    kernel_current_entity_index rq thread_id generation = Some current_index ->
    eevdf_requeue_running rq thread_id = ok rq' ->
    kernel_global_thread_ids_unique
      (fst (kernel_finish_current sched cpu_idx)).
Proof.
  intros sched cpu_idx thread_id generation rq current_index rq'
    Hinv Hvalid Hcurrent Hlookup Hcurrent_entity Hrequeue.
  destruct Hinv as
    [Hshape
      [Hrqs
      [Hunique
      [Hcurrent_matches
      [Hrunning
      [Hnodup Htargets]]]]]].
  pose proof
    (conj Hshape
      (conj Hrqs
      (conj Hunique
      (conj Hcurrent_matches
      (conj Hrunning
      (conj Hnodup Htargets)))))) as Hinv_full.
  unfold kernel_finish_current.
  rewrite Hvalid.
  rewrite Hcurrent.
  rewrite Hlookup.
  rewrite Hcurrent_entity.
  rewrite Hrequeue.
  simpl.
  unfold kernel_global_thread_ids_unique.
  intros cpu_a index_a cpu_b index_b lhs rhs Hlhs Hrhs Hthread Heq.
  destruct (kernel_finish_current_success_entity_on_cpu_original
    sched cpu_idx thread_id generation rq current_index rq'
    cpu_a index_a lhs
    Hinv_full Hvalid Hcurrent Hlookup Hcurrent_entity Hrequeue Hlhs) as
    [orig_lhs [Horig_lhs [Horig_lhs_thread _Horig_lhs_gen]]].
  destruct (kernel_finish_current_success_entity_on_cpu_original
    sched cpu_idx thread_id generation rq current_index rq'
    cpu_b index_b rhs
    Hinv_full Hvalid Hcurrent Hlookup Hcurrent_entity Hrequeue Hrhs) as
    [orig_rhs [Horig_rhs [Horig_rhs_thread _Horig_rhs_gen]]].
  apply Hunique with (lhs := orig_lhs) (rhs := orig_rhs); auto.
  - rewrite Horig_lhs_thread.
    exact Hthread.
  - rewrite Horig_lhs_thread.
    rewrite Horig_rhs_thread.
    exact Heq.
Qed.

Theorem kernel_finish_current_success_preserves_running_entity_has_current :
  forall sched cpu_idx thread_id generation rq current_index rq',
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_cpu_current sched cpu_idx = Some (thread_id, generation) ->
    kernel_lookup_runqueue sched cpu_idx = Some rq ->
    kernel_current_entity_index rq thread_id generation = Some current_index ->
    eevdf_requeue_running rq thread_id = ok rq' ->
    kernel_running_entity_has_current
      (fst (kernel_finish_current sched cpu_idx)).
Proof.
  intros sched cpu_idx thread_id generation rq current_index rq'
    Hinv Hvalid Hcurrent Hlookup Hcurrent_entity Hrequeue.
  destruct Hinv as
    [Hshape
      [Hrqs
      [Hunique
      [Hcurrent_matches
      [Hrunning
      [Hnodup Htargets]]]]]].
  pose proof
    (conj Hshape
      (conj Hrqs
      (conj Hunique
      (conj Hcurrent_matches
      (conj Hrunning
      (conj Hnodup Htargets)))))) as Hinv_full.
  assert (Htarget_active : kernel_active_runqueue sched cpu_idx rq).
  {
    unfold kernel_active_runqueue.
    split.
    - apply kernel_valid_cpu_lt.
      exact Hvalid.
    - unfold kernel_lookup_runqueue in Hlookup.
      exact Hlookup.
  }
  pose proof (Hrqs cpu_idx rq Htarget_active) as Hrq_inv.
  unfold kernel_finish_current.
  rewrite Hvalid.
  rewrite Hcurrent.
  rewrite Hlookup.
  rewrite Hcurrent_entity.
  rewrite Hrequeue.
  simpl.
  unfold kernel_running_entity_has_current.
  intros run_cpu local_rq index entity Hactive_final Hentity_active Hstate.
  destruct (Nat.eq_dec run_cpu cpu_idx) as [Heq | Hneq].
  - subst run_cpu.
    destruct Hactive_final as [Hlt_final Hlookup_final].
    rewrite kernel_clear_cpu_current_preserves_runqueues in Hlookup_final.
    unfold kernel_replace_runqueue in Hlookup_final.
    simpl in Hlookup_final.
    rewrite nth_error_replace_nth_eq in Hlookup_final.
    + inversion Hlookup_final; subst local_rq.
      destruct (eevdf_requeue_running_entity_original_not_target
        rq
        thread_id
        rq'
        index
        entity
        Hrq_inv
        Hrequeue
        Hentity_active
        Hstate) as
        [original
          [Horiginal_active
          [Horiginal_state
          [Horiginal_thread
          [Horiginal_generation Horiginal_not_target]]]]].
      destruct (Hrunning cpu_idx rq index original
        Htarget_active
        Horiginal_active
        Horiginal_state) as
        [cpu [Hslot [Hhas [Htid _Hgen]]]].
      destruct (kernel_cpu_current_slot_matches
        sched
        cpu_idx
        cpu
        thread_id
        generation
	        Hcurrent
	        Hslot) as [_Hcpu_has [Hcpu_thread _Hcpu_generation]].
	      rewrite Htid in Hcpu_thread.
	      exfalso.
	      apply Horiginal_not_target.
	      exact Hcpu_thread.
    + destruct Hshape as [Hcount [Hrqs_len _]].
      rewrite Hrqs_len.
      apply kernel_valid_cpu_lt in Hvalid.
      lia.
  - destruct (Hrunning run_cpu local_rq index entity) as
      [cpu [Hslot [Hhas [Htid Hgen]]]].
    + destruct Hactive_final as [Hlt_final Hlookup_final].
      unfold kernel_active_runqueue.
      rewrite kernel_clear_cpu_current_preserves_cpu_count in Hlt_final.
      simpl in Hlt_final.
      split.
      * exact Hlt_final.
      * rewrite kernel_clear_cpu_current_preserves_runqueues in Hlookup_final.
        unfold kernel_replace_runqueue in Hlookup_final.
        simpl in Hlookup_final.
        rewrite nth_error_replace_nth_neq in Hlookup_final by congruence.
        exact Hlookup_final.
    + exact Hentity_active.
    + exact Hstate.
    + exists cpu.
      split.
      * apply kernel_clear_cpu_current_preserves_other_cpu_slot_forward.
        -- exact Hneq.
        -- apply kernel_replace_runqueue_preserves_cpu_slot_forward.
           exact Hslot.
      * repeat split; auto.
Qed.

Theorem kernel_finish_current_success_preserves_invariant :
  forall sched cpu_idx thread_id generation rq current_index rq',
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_cpu_current sched cpu_idx = Some (thread_id, generation) ->
    kernel_lookup_runqueue sched cpu_idx = Some rq ->
    kernel_current_entity_index rq thread_id generation = Some current_index ->
    eevdf_requeue_running rq thread_id = ok rq' ->
    kernel_sched_invariant
      (fst (kernel_finish_current sched cpu_idx)).
Proof.
  intros sched cpu_idx thread_id generation rq current_index rq'
    Hinv Hvalid Hcurrent Hlookup Hcurrent_entity Hrequeue.
  destruct Hinv as
    [Hshape
      [Hrqs
      [Hunique
      [Hcurrent_matches
      [Hrunning
      [Hnodup Htargets]]]]]].
  pose proof
    (conj Hshape
      (conj Hrqs
      (conj Hunique
      (conj Hcurrent_matches
      (conj Hrunning
      (conj Hnodup Htargets)))))) as Hinv_full.
  split.
  - unfold kernel_finish_current.
    rewrite Hvalid.
    rewrite Hcurrent.
    rewrite Hlookup.
    rewrite Hcurrent_entity.
    rewrite Hrequeue.
    simpl.
    apply kernel_clear_cpu_current_shape.
    apply kernel_replace_runqueue_shape.
    exact Hshape.
  - split.
    + apply kernel_finish_current_success_preserves_runqueues_invariant
        with
          (rq := rq)
          (current_index := current_index)
          (rq' := rq')
          (thread_id := thread_id)
          (generation := generation); auto.
    + split.
      * apply kernel_finish_current_success_preserves_global_thread_ids_unique
          with
            (rq := rq)
            (current_index := current_index)
            (rq' := rq')
            (thread_id := thread_id)
            (generation := generation); auto.
      * split.
        -- apply kernel_finish_current_success_preserves_current_matches
             with
               (rq := rq)
               (current_index := current_index)
               (rq' := rq')
               (thread_id := thread_id)
               (generation := generation); auto.
        -- split.
           ++ apply kernel_finish_current_success_preserves_running_entity_has_current
                with
                  (rq := rq)
                  (current_index := current_index)
                  (rq' := rq')
                  (thread_id := thread_id)
                  (generation := generation); auto.
           ++ split.
              ** apply kernel_finish_current_success_preserves_no_cross_cpu_current_duplicates
                   with
                     (rq := rq)
                     (current_index := current_index)
                     (rq' := rq')
                     (thread_id := thread_id)
                     (generation := generation); auto.
              ** apply kernel_finish_current_success_preserves_activation_targets
                   with
                     (rq := rq)
                     (current_index := current_index)
                     (rq' := rq')
                     (thread_id := thread_id)
                     (generation := generation); auto.
Qed.

Theorem kernel_request_activation_invalid_cpu_preserves_invariant :
  forall sched cpu_idx,
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = false ->
    kernel_sched_invariant
      (fst (kernel_request_activation sched cpu_idx)).
Proof.
  intros sched cpu_idx Hinv Hvalid.
  unfold kernel_request_activation.
  rewrite Hvalid.
  simpl.
  exact Hinv.
Qed.

Theorem kernel_claim_activation_invalid_cpu_preserves_invariant :
  forall sched cpu_idx,
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = false ->
    kernel_sched_invariant
      (fst (kernel_claim_activation sched cpu_idx)).
Proof.
  intros sched cpu_idx Hinv Hvalid.
  unfold kernel_claim_activation.
  rewrite Hvalid.
  simpl.
  exact Hinv.
Qed.

Theorem kernel_claim_activation_idle_success_preserves_invariant :
  forall sched cpu_idx rq rq_after_pick,
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_cpu_has_current sched cpu_idx = false ->
    kernel_lookup_runqueue sched cpu_idx = Some rq ->
    eevdf_pick rq = (rq_after_pick, None) ->
    kernel_sched_invariant
      (fst (kernel_claim_activation sched cpu_idx)).
Proof.
  intros sched cpu_idx rq rq_after_pick Hinv Hvalid Hcurrent Hlookup Hpick.
  unfold kernel_claim_activation.
  rewrite Hvalid.
  apply kernel_pick_idle_success_preserves_invariant
    with (rq := rq) (rq_after_pick := rq_after_pick); auto.
Qed.

Theorem kernel_claim_activation_run_thread_success_preserves_invariant :
  forall sched cpu_idx rq rq_after_pick picked_index picked_entity rq',
    kernel_sched_invariant sched ->
    kernel_valid_cpu sched cpu_idx = true ->
    kernel_cpu_has_current sched cpu_idx = false ->
    kernel_lookup_runqueue sched cpu_idx = Some rq ->
    eevdf_pick rq = (rq_after_pick, Some (picked_index, picked_entity)) ->
    eevdf_mark_running rq_after_pick (ee_thread_id picked_entity) = ok rq' ->
    kernel_sched_invariant
      (fst (kernel_claim_activation sched cpu_idx)).
Proof.
  intros sched cpu_idx rq rq_after_pick picked_index picked_entity rq'
    Hinv Hvalid Hcpu_idle Hlookup Hpick Hmark.
  unfold kernel_claim_activation.
  rewrite Hvalid.
  apply kernel_pick_run_thread_success_preserves_invariant
    with
      (rq := rq)
      (rq_after_pick := rq_after_pick)
      (picked_index := picked_index)
      (picked_entity := picked_entity)
      (rq' := rq'); auto.
Qed.

Theorem kernel_migrate_invalid_cpu_preserves_invariant :
  forall sched src_cpu dst_cpu thread_id,
    kernel_sched_invariant sched ->
    orb
      (negb (kernel_valid_cpu sched src_cpu))
      (negb (kernel_valid_cpu sched dst_cpu)) = true ->
    kernel_sched_invariant
      (fst (kernel_migrate_runnable sched src_cpu dst_cpu thread_id)).
Proof.
  intros sched src_cpu dst_cpu thread_id Hinv Hvalid.
  unfold kernel_migrate_runnable.
  rewrite Hvalid.
  simpl.
  exact Hinv.
Qed.

Theorem kernel_migrate_same_cpu_preserves_invariant :
  forall sched src_cpu thread_id,
    kernel_sched_invariant sched ->
    orb
      (negb (kernel_valid_cpu sched src_cpu))
      (negb (kernel_valid_cpu sched src_cpu)) = false ->
    kernel_sched_invariant
      (fst (kernel_migrate_runnable sched src_cpu src_cpu thread_id)).
Proof.
  intros sched src_cpu thread_id Hinv Hvalid.
  unfold kernel_migrate_runnable.
  rewrite Hvalid.
  rewrite Nat.eqb_refl.
  simpl.
  exact Hinv.
Qed.

Theorem kernel_migrate_missing_thread_preserves_invariant :
  forall sched src_cpu dst_cpu thread_id src dst,
    kernel_sched_invariant sched ->
    orb
      (negb (kernel_valid_cpu sched src_cpu))
      (negb (kernel_valid_cpu sched dst_cpu)) = false ->
    Nat.eqb src_cpu dst_cpu = false ->
    kernel_lookup_runqueue sched src_cpu = Some src ->
    kernel_lookup_runqueue sched dst_cpu = Some dst ->
    find_entity_index src thread_id = None ->
    kernel_sched_invariant
      (fst (kernel_migrate_runnable sched src_cpu dst_cpu thread_id)).
Proof.
  intros sched src_cpu dst_cpu thread_id src dst
    Hinv Hvalid Hneq Hsrc Hdst Hfind.
  unfold kernel_migrate_runnable.
  rewrite Hvalid.
  rewrite Hneq.
  rewrite Hsrc.
  rewrite Hdst.
  rewrite Hfind.
  simpl.
  exact Hinv.
Qed.

Theorem kernel_migrate_non_runnable_preserves_invariant :
  forall sched src_cpu dst_cpu thread_id src dst index entity,
    kernel_sched_invariant sched ->
    orb
      (negb (kernel_valid_cpu sched src_cpu))
      (negb (kernel_valid_cpu sched dst_cpu)) = false ->
    Nat.eqb src_cpu dst_cpu = false ->
    kernel_lookup_runqueue sched src_cpu = Some src ->
    kernel_lookup_runqueue sched dst_cpu = Some dst ->
    find_entity_index src thread_id = Some index ->
    lookup_entity src index = Some entity ->
    ee_state entity <> ERunnable ->
    kernel_sched_invariant
      (fst (kernel_migrate_runnable sched src_cpu dst_cpu thread_id)).
Proof.
  intros sched src_cpu dst_cpu thread_id src dst index entity
    Hinv Hvalid Hneq Hsrc Hdst Hfind Hlookup Hstate.
  unfold kernel_migrate_runnable.
  rewrite Hvalid.
  rewrite Hneq.
  rewrite Hsrc.
  rewrite Hdst.
  rewrite Hfind.
  rewrite Hlookup.
  destruct (ee_state entity); simpl; try exact Hinv; contradiction.
Qed.

Theorem kernel_migrate_destination_full_preserves_invariant :
  forall sched src_cpu dst_cpu thread_id src dst index entity,
    kernel_sched_invariant sched ->
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
    kernel_sched_invariant
      (fst (kernel_migrate_runnable sched src_cpu dst_cpu thread_id)).
Proof.
  intros sched src_cpu dst_cpu thread_id src dst index entity
    Hinv Hvalid Hneq Hsrc Hdst Hfind Hlookup Hstate Hspace.
  unfold kernel_migrate_runnable.
  rewrite Hvalid.
  rewrite Hneq.
  rewrite Hsrc.
  rewrite Hdst.
  rewrite Hfind.
  rewrite Hlookup.
  rewrite Hstate.
  rewrite Hspace.
  simpl.
  exact Hinv.
Qed.

Theorem kernel_migrate_duplicate_destination_preserves_invariant :
  forall sched src_cpu dst_cpu thread_id src dst index entity duplicate_index,
    kernel_sched_invariant sched ->
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
    kernel_sched_invariant
      (fst (kernel_migrate_runnable sched src_cpu dst_cpu thread_id)).
Proof.
  intros sched src_cpu dst_cpu thread_id src dst index entity duplicate_index
    Hinv Hvalid Hneq Hsrc Hdst Hfind Hlookup Hstate Hspace Hdup.
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
  simpl.
  exact Hinv.
Qed.

Theorem kernel_migrate_refresh_overflow_preserves_invariant :
  forall sched src_cpu dst_cpu thread_id src dst index entity,
    kernel_sched_invariant sched ->
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
    kernel_sched_invariant
      (fst (kernel_migrate_runnable sched src_cpu dst_cpu thread_id)).
Proof.
  intros sched src_cpu dst_cpu thread_id src dst index entity
    Hinv Hvalid Hneq Hsrc Hdst Hfind Hlookup Hstate Hspace Hdup Hmoved.
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
  simpl.
  exact Hinv.
Qed.

Theorem kernel_migrate_success_preserves_shape :
  forall sched src_cpu dst_cpu thread_id src dst index entity moved,
    kernel_sched_invariant sched ->
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
    kernel_sched_shape
      (fst (kernel_migrate_runnable sched src_cpu dst_cpu thread_id)).
Proof.
  intros sched src_cpu dst_cpu thread_id src dst index entity moved
    Hinv Hvalid Hneq Hsrc Hdst Hfind Hlookup Hstate Hspace Hdup Hmoved.
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
  simpl.
  destruct Hinv as [Hshape _].
  apply kernel_replace_runqueue_shape.
  apply kernel_replace_runqueue_shape.
  exact Hshape.
Qed.

Theorem kernel_migrate_success_preserves_cpu_slot :
  forall sched src_cpu dst_cpu thread_id src dst index entity moved query_cpu cpu,
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
    kernel_cpu_slot sched query_cpu cpu ->
    kernel_cpu_slot
      (fst (kernel_migrate_runnable sched src_cpu dst_cpu thread_id))
      query_cpu
      cpu.
Proof.
  intros sched src_cpu dst_cpu thread_id src dst index entity moved query_cpu cpu
    Hvalid Hneq Hsrc Hdst Hfind Hlookup Hstate Hspace Hdup Hmoved Hslot.
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
  simpl.
  apply kernel_replace_runqueue_preserves_cpu_slot_forward.
  apply kernel_replace_runqueue_preserves_cpu_slot_forward.
  exact Hslot.
Qed.

Theorem kernel_migrate_success_cpu_slot_original :
  forall sched src_cpu dst_cpu thread_id src dst index entity moved query_cpu cpu,
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
    kernel_cpu_slot
      (fst (kernel_migrate_runnable sched src_cpu dst_cpu thread_id))
      query_cpu
      cpu ->
    kernel_cpu_slot sched query_cpu cpu.
Proof.
  intros sched src_cpu dst_cpu thread_id src dst index entity moved query_cpu cpu
    Hvalid Hneq Hsrc Hdst Hfind Hlookup Hstate Hspace Hdup Hmoved Hslot.
  unfold kernel_migrate_runnable in Hslot.
  rewrite Hvalid in Hslot.
  rewrite Hneq in Hslot.
  rewrite Hsrc in Hslot.
  rewrite Hdst in Hslot.
  rewrite Hfind in Hslot.
  rewrite Hlookup in Hslot.
  rewrite Hstate in Hslot.
  rewrite Hspace in Hslot.
  rewrite Hdup in Hslot.
  rewrite Hmoved in Hslot.
  simpl in Hslot.
  apply kernel_replace_runqueue_preserves_cpu_slot in Hslot.
  apply kernel_replace_runqueue_preserves_cpu_slot in Hslot.
  exact Hslot.
Qed.

Theorem kernel_migrate_success_source_runqueue_active :
  forall sched src_cpu dst_cpu thread_id src dst index entity moved,
    kernel_sched_invariant sched ->
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
    kernel_active_runqueue
      (fst (kernel_migrate_runnable sched src_cpu dst_cpu thread_id))
      src_cpu
      (remove_entity_from_runqueue src thread_id).
Proof.
  intros sched src_cpu dst_cpu thread_id src dst index entity moved
    Hinv Hvalid Hneq Hsrc Hdst Hfind Hlookup Hstate Hspace Hdup Hmoved.
  destruct Hinv as [Hshape _].
  destruct Hshape as [Hcount [Hrqs_len _Hcpus_len]].
  assert (Hsrc_valid : kernel_valid_cpu sched src_cpu = true).
  {
    apply orb_false_iff in Hvalid as [Hsrc_valid Hdst_valid].
    apply negb_false_iff in Hsrc_valid.
    exact Hsrc_valid.
  }
  assert (Hsrc_dst_neq : src_cpu <> dst_cpu).
  {
    apply Nat.eqb_neq.
    exact Hneq.
  }
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
  simpl.
  unfold kernel_active_runqueue, kernel_replace_runqueue.
  simpl.
  split.
  - apply kernel_valid_cpu_lt.
    exact Hsrc_valid.
  - rewrite nth_error_replace_nth_neq by congruence.
    rewrite nth_error_replace_nth_eq.
    + reflexivity.
    + rewrite Hrqs_len.
      apply kernel_valid_cpu_lt in Hsrc_valid.
      lia.
Qed.

Theorem kernel_migrate_success_destination_runqueue_active :
  forall sched src_cpu dst_cpu thread_id src dst index entity moved,
    kernel_sched_invariant sched ->
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
    kernel_active_runqueue
      (fst (kernel_migrate_runnable sched src_cpu dst_cpu thread_id))
      dst_cpu
      (refresh_runqueue (append_entity dst moved)).
Proof.
  intros sched src_cpu dst_cpu thread_id src dst index entity moved
    Hinv Hvalid Hneq Hsrc Hdst Hfind Hlookup Hstate Hspace Hdup Hmoved.
  destruct Hinv as [Hshape _].
  destruct Hshape as [Hcount [Hrqs_len _Hcpus_len]].
  assert (Hdst_valid : kernel_valid_cpu sched dst_cpu = true).
  {
    apply orb_false_iff in Hvalid as [Hsrc_valid Hdst_valid].
    apply negb_false_iff in Hdst_valid.
    exact Hdst_valid.
  }
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
  simpl.
  unfold kernel_active_runqueue, kernel_replace_runqueue.
  simpl.
  split.
  - apply kernel_valid_cpu_lt.
    exact Hdst_valid.
  - rewrite nth_error_replace_nth_eq.
    + reflexivity.
    + rewrite replace_nth_length.
      rewrite Hrqs_len.
      apply kernel_valid_cpu_lt in Hdst_valid.
      lia.
Qed.

Theorem kernel_migrate_success_destination_runqueue_invariant :
  forall sched src_cpu dst_cpu thread_id src dst index entity moved,
    kernel_sched_invariant sched ->
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
    eevdf_invariant (refresh_runqueue (append_entity dst moved)).
Proof.
  intros sched src_cpu dst_cpu thread_id src dst index entity moved
    Hinv Hvalid Hneq Hsrc Hdst Hfind Hlookup Hstate Hspace Hdup Hmoved.
  destruct Hinv as
    [_Hshape
      [Hrqs
      [_Hunique
      [_Hcurrent
      [_Hrunning
      [_Hnodup _Htargets]]]]]].
  assert (Hsrc_active_rq : kernel_active_runqueue sched src_cpu src).
  {
    unfold kernel_active_runqueue.
    split.
    - apply orb_false_iff in Hvalid as [Hsrc_valid _Hdst_valid].
      apply negb_false_iff in Hsrc_valid.
      apply kernel_valid_cpu_lt.
      exact Hsrc_valid.
    - unfold kernel_lookup_runqueue in Hsrc.
      exact Hsrc.
  }
  assert (Hdst_active_rq : kernel_active_runqueue sched dst_cpu dst).
  {
    unfold kernel_active_runqueue.
    split.
    - apply orb_false_iff in Hvalid as [_Hsrc_valid Hdst_valid].
      apply negb_false_iff in Hdst_valid.
      apply kernel_valid_cpu_lt.
      exact Hdst_valid.
    - unfold kernel_lookup_runqueue in Hdst.
      exact Hdst.
  }
  pose proof (Hrqs src_cpu src Hsrc_active_rq) as Hsrc_inv.
  pose proof (Hrqs dst_cpu dst Hdst_active_rq) as Hdst_inv.
  pose proof
    (find_entity_index_lookup_matches_thread
      src
      thread_id
      index
      entity
      Hfind
      Hlookup) as [Hentity_thread Hentity_active_state].
  pose proof
    (find_entity_index_thread_not_none src thread_id index Hfind)
    as Hthread_not_none.
  pose proof
    (find_entity_index_active
      src
      thread_id
      index
      entity
      Hfind
      Hlookup) as Hentity_active.
  destruct Hsrc_inv as
    [_Hsrc_shape
    [_Hsrc_inactive
    [_Hsrc_unique
    [Hsrc_positive
    [_Hsrc_live
    [_Hsrc_min
    [_Hsrc_virtual _Hsrc_runnable]]]]]]].
  destruct (Hsrc_positive index entity Hentity_active Hentity_active_state)
    as [Hentity_weight Hentity_slice].
  pose proof
    (entity_as_migrated_runnable_properties dst entity moved Hmoved)
    as
      [Hmoved_thread
      [_Hmoved_generation
      [Hmoved_weight
      [Hmoved_slice
      [_Hmoved_service
      [_Hmoved_floor Hmoved_deadline]]]]]].
  assert (Hspace_lt : (er_entity_count dst < eevdf_max_entities)%nat)
    by (apply Nat.ltb_lt; exact Hspace).
  apply refresh_append_preserves_invariant with (thread_id := thread_id).
  - exact Hdst_inv.
  - exact Hspace_lt.
  - exact Hthread_not_none.
  - exact Hdup.
  - rewrite Hmoved_thread.
    exact Hentity_thread.
  - rewrite Hmoved_weight.
    exact Hentity_weight.
  - rewrite Hmoved_slice.
    exact Hentity_slice.
  - exact Hmoved_deadline.
Qed.

Theorem kernel_migrate_success_preserves_runqueues_without_thread_uniqueness :
  forall sched src_cpu dst_cpu thread_id src dst index entity moved,
    kernel_sched_invariant sched ->
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
    kernel_runqueues_invariant_without_thread_uniqueness
      (fst (kernel_migrate_runnable sched src_cpu dst_cpu thread_id)).
Proof.
  intros sched src_cpu dst_cpu thread_id src dst index entity moved
    Hinv Hvalid Hneq Hsrc Hdst Hfind Hlookup Hstate Hspace Hdup Hmoved.
  pose proof Hinv as Hinv_full.
  destruct Hinv as
    [_Hshape
      [Hrqs
      [_Hunique
      [_Hcurrent
      [_Hrunning
      [_Hnodup _Htargets]]]]]].
  unfold kernel_runqueues_invariant_without_thread_uniqueness.
  intros query_cpu query_rq Hactive_final.
  destruct (Nat.eq_dec query_cpu src_cpu) as [Hquery_src | Hquery_not_src].
  - subst query_cpu.
    assert (Hquery_is_src :
      query_rq = remove_entity_from_runqueue src thread_id).
    {
      pose proof
        (kernel_migrate_success_source_runqueue_active
          sched
          src_cpu
          dst_cpu
          thread_id
          src
          dst
          index
          entity
          moved
          Hinv_full
          Hvalid
          Hneq
          Hsrc
          Hdst
          Hfind
          Hlookup
          Hstate
          Hspace
          Hdup
          Hmoved) as Hsource_final.
      destruct Hactive_final as [_ Hquery_lookup].
      destruct Hsource_final as [_ Hsource_lookup].
      rewrite Hquery_lookup in Hsource_lookup.
      inversion Hsource_lookup.
      reflexivity.
    }
    subst query_rq.
    assert (Hsrc_active : kernel_active_runqueue sched src_cpu src).
    {
      unfold kernel_active_runqueue.
      split.
      - apply orb_false_iff in Hvalid as [Hsrc_valid _Hdst_valid].
        apply negb_false_iff in Hsrc_valid.
        apply kernel_valid_cpu_lt.
        exact Hsrc_valid.
      - unfold kernel_lookup_runqueue in Hsrc.
        exact Hsrc.
    }
    apply remove_entity_from_runqueue_preserves_basic_safety.
    exact (Hrqs src_cpu src Hsrc_active).
  - destruct (Nat.eq_dec query_cpu dst_cpu) as [Hquery_dst | Hquery_not_dst].
    + subst query_cpu.
      assert (Hquery_is_dst :
        query_rq = refresh_runqueue (append_entity dst moved)).
      {
        pose proof
          (kernel_migrate_success_destination_runqueue_active
            sched
            src_cpu
            dst_cpu
            thread_id
            src
            dst
            index
            entity
            moved
            Hinv_full
            Hvalid
            Hneq
            Hsrc
            Hdst
            Hfind
            Hlookup
            Hstate
            Hspace
            Hdup
            Hmoved) as Hdestination_final.
        destruct Hactive_final as [_ Hquery_lookup].
        destruct Hdestination_final as [_ Hdestination_lookup].
        rewrite Hquery_lookup in Hdestination_lookup.
        inversion Hdestination_lookup.
        reflexivity.
      }
      subst query_rq.
      apply eevdf_invariant_implies_without_thread_uniqueness.
      apply kernel_migrate_success_destination_runqueue_invariant with
        (sched := sched)
        (src_cpu := src_cpu)
        (dst_cpu := dst_cpu)
        (thread_id := thread_id)
        (src := src)
        (dst := dst)
        (index := index)
        (entity := entity);
        auto.
    + assert (Hactive_original : kernel_active_runqueue sched query_cpu query_rq).
      {
        destruct Hactive_final as [Hlt Hlookup_final].
        unfold kernel_active_runqueue.
        split.
        - unfold kernel_migrate_runnable in Hlt.
          rewrite Hvalid in Hlt.
          rewrite Hneq in Hlt.
          rewrite Hsrc in Hlt.
          rewrite Hdst in Hlt.
          rewrite Hfind in Hlt.
          rewrite Hlookup in Hlt.
          rewrite Hstate in Hlt.
          rewrite Hspace in Hlt.
          rewrite Hdup in Hlt.
          rewrite Hmoved in Hlt.
          simpl in Hlt.
          unfold kernel_replace_runqueue in Hlt.
          simpl in Hlt.
          exact Hlt.
        - unfold kernel_migrate_runnable in Hlookup_final.
          rewrite Hvalid in Hlookup_final.
          rewrite Hneq in Hlookup_final.
          rewrite Hsrc in Hlookup_final.
          rewrite Hdst in Hlookup_final.
          rewrite Hfind in Hlookup_final.
          rewrite Hlookup in Hlookup_final.
          rewrite Hstate in Hlookup_final.
          rewrite Hspace in Hlookup_final.
          rewrite Hdup in Hlookup_final.
          rewrite Hmoved in Hlookup_final.
          simpl in Hlookup_final.
          unfold kernel_replace_runqueue in Hlookup_final.
          simpl in Hlookup_final.
          rewrite nth_error_replace_nth_neq in Hlookup_final by congruence.
          rewrite nth_error_replace_nth_neq in Hlookup_final by congruence.
          exact Hlookup_final.
      }
      apply eevdf_invariant_implies_without_thread_uniqueness.
      exact (Hrqs query_cpu query_rq Hactive_original).
Qed.

Theorem kernel_migrate_success_preserves_runqueues_invariant :
  forall sched src_cpu dst_cpu thread_id src dst index entity moved,
    kernel_sched_invariant sched ->
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
    kernel_runqueues_invariant
      (fst (kernel_migrate_runnable sched src_cpu dst_cpu thread_id)).
Proof.
  intros sched src_cpu dst_cpu thread_id src dst index entity moved
    Hinv Hvalid Hneq Hsrc Hdst Hfind Hlookup Hstate Hspace Hdup Hmoved.
  pose proof Hinv as Hinv_full.
  destruct Hinv as
    [_Hshape
      [Hrqs
      [_Hunique
      [_Hcurrent
      [_Hrunning
      [_Hnodup _Htargets]]]]]].
  unfold kernel_runqueues_invariant.
  intros query_cpu query_rq Hactive_final.
  destruct (Nat.eq_dec query_cpu src_cpu) as [Hquery_src | Hquery_not_src].
  - subst query_cpu.
    assert (Hquery_is_src :
      query_rq = remove_entity_from_runqueue src thread_id).
    {
      pose proof
        (kernel_migrate_success_source_runqueue_active
          sched
          src_cpu
          dst_cpu
          thread_id
          src
          dst
          index
          entity
          moved
          Hinv_full
          Hvalid
          Hneq
          Hsrc
          Hdst
          Hfind
          Hlookup
          Hstate
          Hspace
          Hdup
          Hmoved) as Hsource_final.
      destruct Hactive_final as [_ Hquery_lookup].
      destruct Hsource_final as [_ Hsource_lookup].
      rewrite Hquery_lookup in Hsource_lookup.
      inversion Hsource_lookup.
      reflexivity.
    }
    subst query_rq.
    assert (Hsrc_active : kernel_active_runqueue sched src_cpu src).
    {
      unfold kernel_active_runqueue.
      split.
      - apply orb_false_iff in Hvalid as [Hsrc_valid _Hdst_valid].
        apply negb_false_iff in Hsrc_valid.
        apply kernel_valid_cpu_lt.
        exact Hsrc_valid.
      - unfold kernel_lookup_runqueue in Hsrc.
        exact Hsrc.
    }
    apply remove_entity_from_runqueue_preserves_invariant.
    exact (Hrqs src_cpu src Hsrc_active).
  - destruct (Nat.eq_dec query_cpu dst_cpu) as [Hquery_dst | Hquery_not_dst].
    + subst query_cpu.
      assert (Hquery_is_dst :
        query_rq = refresh_runqueue (append_entity dst moved)).
      {
        pose proof
          (kernel_migrate_success_destination_runqueue_active
            sched
            src_cpu
            dst_cpu
            thread_id
            src
            dst
            index
            entity
            moved
            Hinv_full
            Hvalid
            Hneq
            Hsrc
            Hdst
            Hfind
            Hlookup
            Hstate
            Hspace
            Hdup
            Hmoved) as Hdestination_final.
        destruct Hactive_final as [_ Hquery_lookup].
        destruct Hdestination_final as [_ Hdestination_lookup].
        rewrite Hquery_lookup in Hdestination_lookup.
        inversion Hdestination_lookup.
        reflexivity.
      }
      subst query_rq.
      apply kernel_migrate_success_destination_runqueue_invariant with
        (sched := sched)
        (src_cpu := src_cpu)
        (dst_cpu := dst_cpu)
        (thread_id := thread_id)
        (src := src)
        (dst := dst)
        (index := index)
        (entity := entity);
        auto.
    + assert (Hactive_original : kernel_active_runqueue sched query_cpu query_rq).
      {
        destruct Hactive_final as [Hlt Hlookup_final].
        unfold kernel_active_runqueue.
        split.
        - unfold kernel_migrate_runnable in Hlt.
          rewrite Hvalid in Hlt.
          rewrite Hneq in Hlt.
          rewrite Hsrc in Hlt.
          rewrite Hdst in Hlt.
          rewrite Hfind in Hlt.
          rewrite Hlookup in Hlt.
          rewrite Hstate in Hlt.
          rewrite Hspace in Hlt.
          rewrite Hdup in Hlt.
          rewrite Hmoved in Hlt.
          simpl in Hlt.
          unfold kernel_replace_runqueue in Hlt.
          simpl in Hlt.
          exact Hlt.
        - unfold kernel_migrate_runnable in Hlookup_final.
          rewrite Hvalid in Hlookup_final.
          rewrite Hneq in Hlookup_final.
          rewrite Hsrc in Hlookup_final.
          rewrite Hdst in Hlookup_final.
          rewrite Hfind in Hlookup_final.
          rewrite Hlookup in Hlookup_final.
          rewrite Hstate in Hlookup_final.
          rewrite Hspace in Hlookup_final.
          rewrite Hdup in Hlookup_final.
          rewrite Hmoved in Hlookup_final.
          simpl in Hlookup_final.
          unfold kernel_replace_runqueue in Hlookup_final.
          simpl in Hlookup_final.
          rewrite nth_error_replace_nth_neq in Hlookup_final by congruence.
          rewrite nth_error_replace_nth_neq in Hlookup_final by congruence.
          exact Hlookup_final.
      }
      exact (Hrqs query_cpu query_rq Hactive_original).
Qed.

Theorem kernel_migrate_success_entity_origin_or_moved :
  forall sched src_cpu dst_cpu thread_id src dst index entity moved
    final_cpu final_index final_entity,
    kernel_sched_invariant sched ->
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
    kernel_entity_on_cpu
      (fst (kernel_migrate_runnable sched src_cpu dst_cpu thread_id))
      final_cpu
      final_index
      final_entity ->
    (exists original_index,
      kernel_entity_on_cpu sched final_cpu original_index final_entity /\
      ee_thread_id final_entity <> thread_id) \/
    (final_cpu = dst_cpu /\
      final_index = er_entity_count dst /\
      final_entity = moved /\
      ee_thread_id moved = thread_id).
Proof.
  intros sched src_cpu dst_cpu thread_id src dst index entity moved
    final_cpu final_index final_entity
    Hinv Hvalid Hneq Hsrc Hdst Hfind Hlookup Hstate Hspace Hdup Hmoved
    Hfinal_entity.
  pose proof Hinv as Hinv_full.
  destruct Hfinal_entity as
    [final_rq [Hfinal_rq [Hfinal_active Hfinal_active_state]]].
  assert (Hthread_not_none : thread_id <> no_thread_id).
  {
    apply find_entity_index_thread_not_none with (rq := src) (index := index).
    exact Hfind.
  }
  destruct Hinv as
    [_Hshape
      [Hrqs
      [_Hunique
      [_Hcurrent
      [_Hrunning
      [_Hnodup _Htargets]]]]]].
  assert (Hdst_active_rq : kernel_active_runqueue sched dst_cpu dst).
  {
    unfold kernel_active_runqueue.
    split.
    - apply orb_false_iff in Hvalid as [_Hsrc_valid Hdst_valid].
      apply negb_false_iff in Hdst_valid.
      apply kernel_valid_cpu_lt.
      exact Hdst_valid.
    - unfold kernel_lookup_runqueue in Hdst.
      exact Hdst.
  }
  pose proof (Hrqs dst_cpu dst Hdst_active_rq) as Hdst_inv.
  destruct Hdst_inv as [Hdst_shape _].
  destruct (Nat.eq_dec final_cpu src_cpu) as [Hfinal_src | Hfinal_not_src].
  - subst final_cpu.
    left.
    assert (Hfinal_is_src :
      final_rq = remove_entity_from_runqueue src thread_id).
    {
      pose proof
        (kernel_migrate_success_source_runqueue_active
          sched
          src_cpu
          dst_cpu
          thread_id
          src
          dst
          index
          entity
          moved
          Hinv_full
          Hvalid
          Hneq
          Hsrc
          Hdst
          Hfind
          Hlookup
          Hstate
          Hspace
          Hdup
          Hmoved) as Hsource_final.
      destruct Hfinal_rq as [_ Hfinal_lookup].
      destruct Hsource_final as [_ Hsource_lookup].
      rewrite Hfinal_lookup in Hsource_lookup.
      inversion Hsource_lookup.
      reflexivity.
    }
    subst final_rq.
    destruct (remove_entity_from_runqueue_active_from_original
      src
      thread_id
      final_index
      final_entity) as [original_index [Horiginal_active Hkept]].
    + destruct (Hrqs src_cpu src) as
        [Hsrc_shape _Hsrc_rest].
      * unfold kernel_active_runqueue.
        split.
        -- apply orb_false_iff in Hvalid as [Hsrc_valid _Hdst_valid].
           apply negb_false_iff in Hsrc_valid.
           apply kernel_valid_cpu_lt.
           exact Hsrc_valid.
        -- unfold kernel_lookup_runqueue in Hsrc.
           exact Hsrc.
      * exact Hsrc_shape.
    + exact Hfinal_active.
    + exists original_index.
      split.
      * unfold kernel_entity_on_cpu.
        exists src.
        split.
        -- unfold kernel_active_runqueue.
           split.
           ++ apply orb_false_iff in Hvalid as [Hsrc_valid _Hdst_valid].
              apply negb_false_iff in Hsrc_valid.
              apply kernel_valid_cpu_lt.
              exact Hsrc_valid.
           ++ unfold kernel_lookup_runqueue in Hsrc.
              exact Hsrc.
        -- split; auto.
      * exact Hkept.
  - destruct (Nat.eq_dec final_cpu dst_cpu) as [Hfinal_dst | Hfinal_not_dst].
    + subst final_cpu.
      assert (Hfinal_is_dst :
        final_rq = refresh_runqueue (append_entity dst moved)).
      {
        pose proof
          (kernel_migrate_success_destination_runqueue_active
            sched
            src_cpu
            dst_cpu
            thread_id
            src
            dst
            index
            entity
            moved
            Hinv_full
            Hvalid
            Hneq
            Hsrc
            Hdst
            Hfind
            Hlookup
            Hstate
            Hspace
            Hdup
            Hmoved) as Hdestination_final.
        destruct Hfinal_rq as [_ Hfinal_lookup].
        destruct Hdestination_final as [_ Hdestination_lookup].
        rewrite Hfinal_lookup in Hdestination_lookup.
        inversion Hdestination_lookup.
        reflexivity.
      }
      subst final_rq.
      pose proof Hfinal_active as Happend_active.
      apply (proj1 (refresh_active_entity_iff _ _ _)) in Happend_active.
      assert (Hspace_lt : (er_entity_count dst < eevdf_max_entities)%nat)
        by (apply Nat.ltb_lt; exact Hspace).
      pose proof (append_entity_active_cases_shaped
        dst
        moved
        final_index
        final_entity
        Hdst_shape
        Hspace_lt
        Happend_active) as Hcases.
      destruct Hcases as [[Hnew_index Hnew_entity] | [_Hold_index Hold_active]].
      * right.
        subst final_index final_entity.
        pose proof
          (entity_as_migrated_runnable_properties dst entity moved Hmoved)
          as [Hmoved_thread _Hmoved_rest].
        repeat split; auto.
        rewrite Hmoved_thread.
        pose proof
          (find_entity_index_lookup_matches_thread
            src
            thread_id
            index
            entity
            Hfind
            Hlookup) as [Hentity_thread _Hentity_active].
        exact Hentity_thread.
      * left.
        assert (Hold_thread : ee_thread_id final_entity <> thread_id).
        {
          apply find_entity_index_none_no_active_thread with
            (rq := dst)
            (index := final_index);
            auto.
        }
        exists final_index.
        split.
        -- unfold kernel_entity_on_cpu.
           exists dst.
           split.
           ++ exact Hdst_active_rq.
           ++ split; auto.
        -- exact Hold_thread.
    + left.
      assert (Horiginal_rq : kernel_active_runqueue sched final_cpu final_rq).
      {
        destruct Hfinal_rq as [Hlt Hlookup_final].
        unfold kernel_active_runqueue.
        split.
        - unfold kernel_migrate_runnable in Hlt.
          rewrite Hvalid in Hlt.
          rewrite Hneq in Hlt.
          rewrite Hsrc in Hlt.
          rewrite Hdst in Hlt.
          rewrite Hfind in Hlt.
          rewrite Hlookup in Hlt.
          rewrite Hstate in Hlt.
          rewrite Hspace in Hlt.
          rewrite Hdup in Hlt.
          rewrite Hmoved in Hlt.
          simpl in Hlt.
          unfold kernel_replace_runqueue in Hlt.
          simpl in Hlt.
          exact Hlt.
        - unfold kernel_migrate_runnable in Hlookup_final.
          rewrite Hvalid in Hlookup_final.
          rewrite Hneq in Hlookup_final.
          rewrite Hsrc in Hlookup_final.
          rewrite Hdst in Hlookup_final.
          rewrite Hfind in Hlookup_final.
          rewrite Hlookup in Hlookup_final.
          rewrite Hstate in Hlookup_final.
          rewrite Hspace in Hlookup_final.
          rewrite Hdup in Hlookup_final.
          rewrite Hmoved in Hlookup_final.
          simpl in Hlookup_final.
          unfold kernel_replace_runqueue in Hlookup_final.
          simpl in Hlookup_final.
          rewrite nth_error_replace_nth_neq in Hlookup_final by congruence.
          rewrite nth_error_replace_nth_neq in Hlookup_final by congruence.
          exact Hlookup_final.
      }
      assert (Hnot_moved_thread : ee_thread_id final_entity <> thread_id).
      {
        intros Hthread_eq.
        pose proof
          (find_entity_index_lookup_matches_thread
            src
            thread_id
            index
            entity
            Hfind
            Hlookup) as [Hentity_thread Hentity_active].
        assert (Hsrc_entity_on :
          kernel_entity_on_cpu sched src_cpu index entity).
        {
          unfold kernel_entity_on_cpu.
          exists src.
          split.
          - unfold kernel_active_runqueue.
            split.
            + apply orb_false_iff in Hvalid as [Hsrc_valid _Hdst_valid].
              apply negb_false_iff in Hsrc_valid.
              apply kernel_valid_cpu_lt.
              exact Hsrc_valid.
            + unfold kernel_lookup_runqueue in Hsrc.
              exact Hsrc.
          - split.
            + apply find_entity_index_active with (thread_id := thread_id);
                auto.
            + rewrite Hstate.
              reflexivity.
        }
        assert (Hother_entity_on :
          kernel_entity_on_cpu sched final_cpu final_index final_entity).
        {
          unfold kernel_entity_on_cpu.
          exists final_rq.
          split; auto.
        }
        destruct Hinv_full as
          [_Hshape_full
          [_Hrqs_full
          [Hunique_full _Hrest_full]]].
        destruct (Hunique_full
          src_cpu
          index
          final_cpu
          final_index
          entity
          final_entity
          Hsrc_entity_on
          Hother_entity_on) as [Hcpu_eq _Hindex_eq].
        - rewrite Hentity_thread.
          exact Hthread_not_none.
        - rewrite Hentity_thread.
          symmetry.
          exact Hthread_eq.
        - symmetry in Hcpu_eq.
          contradiction.
      }
      exists final_index.
      split.
      * unfold kernel_entity_on_cpu.
        exists final_rq.
        split; auto.
      * exact Hnot_moved_thread.
Qed.

Theorem kernel_migrate_success_preserves_global_thread_ids_unique :
  forall sched src_cpu dst_cpu thread_id src dst index entity moved,
    kernel_sched_invariant sched ->
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
    kernel_global_thread_ids_unique
      (fst (kernel_migrate_runnable sched src_cpu dst_cpu thread_id)).
Proof.
  intros sched src_cpu dst_cpu thread_id src dst index entity moved
    Hinv Hvalid Hneq Hsrc Hdst Hfind Hlookup Hstate Hspace Hdup Hmoved.
  pose proof Hinv as Hinv_full.
  destruct Hinv as
    [_Hshape
      [_Hrqs
      [Hunique
      [_Hcurrent
      [_Hrunning
      [_Hnodup _Htargets]]]]]].
  unfold kernel_global_thread_ids_unique.
  intros cpu_a index_a cpu_b index_b lhs rhs
    Hlhs_final Hrhs_final Hlhs_nonzero Hthreads_eq.
  pose proof
    (kernel_migrate_success_entity_origin_or_moved
      sched src_cpu dst_cpu thread_id src dst index entity moved
      cpu_a index_a lhs
      Hinv_full Hvalid Hneq Hsrc Hdst Hfind Hlookup Hstate Hspace Hdup Hmoved
      Hlhs_final) as Hlhs_case.
  pose proof
    (kernel_migrate_success_entity_origin_or_moved
      sched src_cpu dst_cpu thread_id src dst index entity moved
      cpu_b index_b rhs
      Hinv_full Hvalid Hneq Hsrc Hdst Hfind Hlookup Hstate Hspace Hdup Hmoved
      Hrhs_final) as Hrhs_case.
  assert (Hcpu_eq : cpu_a = cpu_b).
  {
    destruct Hlhs_case as
      [[lhs_original_index [Hlhs_original Hlhs_not_moved]] |
       [Hlhs_cpu [Hlhs_index [Hlhs_entity Hlhs_thread]]]];
    destruct Hrhs_case as
      [[rhs_original_index [Hrhs_original Hrhs_not_moved]] |
       [Hrhs_cpu [Hrhs_index [Hrhs_entity Hrhs_thread]]]].
    - destruct (Hunique
        cpu_a
        lhs_original_index
        cpu_b
        rhs_original_index
        lhs
        rhs
        Hlhs_original
        Hrhs_original
        Hlhs_nonzero
        Hthreads_eq) as [Hcpu _Hindex].
      exact Hcpu.
    - subst cpu_b rhs.
      rewrite Hrhs_thread in Hthreads_eq.
      contradiction.
    - subst cpu_a lhs.
      rewrite Hlhs_thread in Hthreads_eq.
      symmetry in Hthreads_eq.
      contradiction.
    - subst cpu_a cpu_b.
      reflexivity.
  }
  subst cpu_b.
  split; [reflexivity |].
  destruct Hlhs_final as [lhs_rq [Hlhs_rq [Hlhs_active Hlhs_active_state]]].
  destruct Hrhs_final as [rhs_rq [Hrhs_rq [Hrhs_active Hrhs_active_state]]].
  assert (Hsame_rq : lhs_rq = rhs_rq).
  {
    destruct Hlhs_rq as [_ Hlhs_lookup].
    destruct Hrhs_rq as [_ Hrhs_lookup].
    rewrite Hlhs_lookup in Hrhs_lookup.
    inversion Hrhs_lookup.
    reflexivity.
  }
  subst rhs_rq.
  pose proof
    (kernel_migrate_success_preserves_runqueues_invariant
      sched src_cpu dst_cpu thread_id src dst index entity moved
      Hinv_full Hvalid Hneq Hsrc Hdst Hfind Hlookup Hstate Hspace Hdup Hmoved)
    as Hrqs_final.
  pose proof (Hrqs_final cpu_a lhs_rq Hlhs_rq) as Hlhs_rq_inv.
  destruct Hlhs_rq_inv as
    [_Hshape_final
    [_Hinactive_final
    [Hlocal_unique _Hrest_final]]].
  eapply Hlocal_unique; eauto.
Qed.

Theorem kernel_migrate_success_preserves_current_matches :
  forall sched src_cpu dst_cpu thread_id src dst index entity moved,
    kernel_sched_invariant sched ->
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
    kernel_current_matches_local_running
      (fst (kernel_migrate_runnable sched src_cpu dst_cpu thread_id)).
Proof.
  intros sched src_cpu dst_cpu thread_id src dst index entity moved
    Hinv Hvalid Hneq Hsrc Hdst Hfind Hlookup Hstate Hspace Hdup Hmoved.
  pose proof Hinv as Hinv_full.
  destruct Hinv as
    [Hshape
      [Hrqs
      [_Hunique
      [Hcurrent
      [_Hrunning
      [_Hnodup _Htargets]]]]]].
  assert (Hsrc_active : kernel_active_runqueue sched src_cpu src).
  {
    unfold kernel_active_runqueue.
    split.
    - apply orb_false_iff in Hvalid as [Hsrc_valid _Hdst_valid].
      apply negb_false_iff in Hsrc_valid.
      apply kernel_valid_cpu_lt.
      exact Hsrc_valid.
    - unfold kernel_lookup_runqueue in Hsrc.
      exact Hsrc.
  }
  assert (Hdst_active : kernel_active_runqueue sched dst_cpu dst).
  {
    unfold kernel_active_runqueue.
    split.
    - apply orb_false_iff in Hvalid as [_Hsrc_valid Hdst_valid].
      apply negb_false_iff in Hdst_valid.
      apply kernel_valid_cpu_lt.
      exact Hdst_valid.
    - unfold kernel_lookup_runqueue in Hdst.
      exact Hdst.
  }
  pose proof (Hrqs src_cpu src Hsrc_active) as Hsrc_inv.
  pose proof (Hrqs dst_cpu dst Hdst_active) as Hdst_inv.
  destruct Hsrc_inv as [Hsrc_shape _].
  destruct Hdst_inv as [Hdst_shape _].
  unfold kernel_current_matches_local_running.
  intros query_cpu cpu Hslot_final Hhas.
  pose proof
    (kernel_migrate_success_cpu_slot_original
      sched
      src_cpu
      dst_cpu
      thread_id
      src
      dst
      index
      entity
      moved
      query_cpu
      cpu
      Hvalid
      Hneq
      Hsrc
      Hdst
      Hfind
      Hlookup
      Hstate
      Hspace
      Hdup
      Hmoved
      Hslot_final) as Hslot_original.
  destruct (Hcurrent query_cpu cpu Hslot_original Hhas)
    as [original_rq [original_index [running
      [Horiginal_rq [Hrunning_active
        [Hrunning_state [Hrunning_thread Hrunning_generation]]]]]]].
  destruct (Nat.eq_dec query_cpu src_cpu) as [Hquery_src | Hquery_not_src].
  - subst query_cpu.
    assert (Horiginal_is_src : original_rq = src).
    {
      destruct Horiginal_rq as [_ Horiginal_lookup].
      destruct Hsrc_active as [_ Hsrc_lookup].
      rewrite Horiginal_lookup in Hsrc_lookup.
      inversion Hsrc_lookup.
      reflexivity.
    }
    subst original_rq.
    assert (Hrunning_not_migrated : ee_thread_id running <> thread_id).
    {
      apply kernel_migrating_runnable_differs_from_running_entity
        with
          (sched := sched)
          (src_cpu := src_cpu)
          (src := src)
          (migrate_index := index)
          (migrated := entity)
          (run_cpu := src_cpu)
          (run_rq := src)
          (run_index := original_index);
        auto.
    }
    destruct (remove_entity_from_runqueue_keeps_other_active
      src
      thread_id
      original_index
      running
      Hsrc_shape
      Hrunning_active
      Hrunning_not_migrated) as [final_index Hfinal_entity].
    exists (remove_entity_from_runqueue src thread_id), final_index, running.
    split.
    + apply kernel_migrate_success_source_runqueue_active with
        (sched := sched)
        (dst := dst)
        (index := index)
        (entity := entity)
        (moved := moved);
        auto.
    + split.
      * exact Hfinal_entity.
      * repeat split; auto.
  - destruct (Nat.eq_dec query_cpu dst_cpu) as [Hquery_dst | Hquery_not_dst].
    + subst query_cpu.
      assert (Horiginal_is_dst : original_rq = dst).
      {
        destruct Horiginal_rq as [_ Horiginal_lookup].
        destruct Hdst_active as [_ Hdst_lookup].
        rewrite Horiginal_lookup in Hdst_lookup.
        inversion Hdst_lookup.
        reflexivity.
      }
      subst original_rq.
      assert (Hfinal_entity :
        eevdf_active_entity
          (refresh_runqueue (append_entity dst moved))
          original_index
          running).
      {
        apply refresh_append_entity_keeps_existing_active; auto.
      }
      exists (refresh_runqueue (append_entity dst moved)), original_index, running.
      split.
      * apply kernel_migrate_success_destination_runqueue_active with
          (sched := sched)
          (src := src)
          (index := index)
          (entity := entity)
          (moved := moved);
          auto.
      * split.
        -- exact Hfinal_entity.
        -- repeat split; auto.
    + exists original_rq, original_index, running.
      split.
      * destruct Horiginal_rq as [Hlt Hlookup_original].
        unfold kernel_active_runqueue.
        split.
        -- unfold kernel_migrate_runnable.
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
           simpl.
           unfold kernel_replace_runqueue.
           simpl.
           exact Hlt.
        -- unfold kernel_migrate_runnable.
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
           simpl.
           unfold kernel_replace_runqueue.
           simpl.
           rewrite nth_error_replace_nth_neq by congruence.
           rewrite nth_error_replace_nth_neq by congruence.
           exact Hlookup_original.
      * split.
        -- exact Hrunning_active.
        -- repeat split; auto.
Qed.

Theorem kernel_migrate_success_preserves_running_entity_has_current :
  forall sched src_cpu dst_cpu thread_id src dst index entity moved,
    kernel_sched_invariant sched ->
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
    kernel_running_entity_has_current
      (fst (kernel_migrate_runnable sched src_cpu dst_cpu thread_id)).
Proof.
  intros sched src_cpu dst_cpu thread_id src dst index entity moved
    Hinv Hvalid Hneq Hsrc Hdst Hfind Hlookup Hstate Hspace Hdup Hmoved.
  pose proof Hinv as Hinv_full.
  destruct Hinv as
    [Hshape
      [Hrqs
      [_Hunique
      [_Hcurrent
      [Hrunning
      [_Hnodup _Htargets]]]]]].
  assert (Hsrc_active : kernel_active_runqueue sched src_cpu src).
  {
    unfold kernel_active_runqueue.
    split.
    - apply orb_false_iff in Hvalid as [Hsrc_valid _Hdst_valid].
      apply negb_false_iff in Hsrc_valid.
      apply kernel_valid_cpu_lt.
      exact Hsrc_valid.
    - unfold kernel_lookup_runqueue in Hsrc.
      exact Hsrc.
  }
  assert (Hdst_active : kernel_active_runqueue sched dst_cpu dst).
  {
    unfold kernel_active_runqueue.
    split.
    - apply orb_false_iff in Hvalid as [_Hsrc_valid Hdst_valid].
      apply negb_false_iff in Hdst_valid.
      apply kernel_valid_cpu_lt.
      exact Hdst_valid.
    - unfold kernel_lookup_runqueue in Hdst.
      exact Hdst.
  }
  pose proof (Hrqs src_cpu src Hsrc_active) as Hsrc_inv.
  pose proof (Hrqs dst_cpu dst Hdst_active) as Hdst_inv.
  destruct Hsrc_inv as [Hsrc_shape _].
  destruct Hdst_inv as [Hdst_shape _].
  unfold kernel_running_entity_has_current.
  intros entity_cpu final_rq final_index running
    Hfinal_rq Hfinal_active Hrunning_state.
  destruct (Nat.eq_dec entity_cpu src_cpu) as [Hentity_src | Hentity_not_src].
  - subst entity_cpu.
    assert (Hfinal_is_src :
      final_rq = remove_entity_from_runqueue src thread_id).
    {
      pose proof
        (kernel_migrate_success_source_runqueue_active
          sched
          src_cpu
          dst_cpu
          thread_id
          src
          dst
          index
          entity
          moved
          Hinv_full
          Hvalid
          Hneq
          Hsrc
          Hdst
          Hfind
          Hlookup
          Hstate
          Hspace
          Hdup
          Hmoved) as Hsource_final.
      destruct Hfinal_rq as [_ Hfinal_lookup].
      destruct Hsource_final as [_ Hsource_lookup].
      rewrite Hfinal_lookup in Hsource_lookup.
      inversion Hsource_lookup.
      reflexivity.
    }
    subst final_rq.
    destruct (remove_entity_from_runqueue_active_from_original
      src
      thread_id
      final_index
      running
      Hsrc_shape
      Hfinal_active) as [original_index [Horiginal_active _Hnot_removed]].
    destruct (Hrunning
      src_cpu
      src
      original_index
      running
      Hsrc_active
      Horiginal_active
      Hrunning_state) as [cpu [Hslot [Hhas [Hthread Hgen]]]].
    exists cpu.
    split.
    + apply kernel_migrate_success_preserves_cpu_slot with
        (sched := sched)
        (src := src)
        (dst := dst)
        (index := index)
        (entity := entity)
        (moved := moved);
        auto.
    + repeat split; auto.
  - destruct (Nat.eq_dec entity_cpu dst_cpu) as [Hentity_dst | Hentity_not_dst].
    + subst entity_cpu.
      assert (Hfinal_is_dst :
        final_rq = refresh_runqueue (append_entity dst moved)).
      {
        pose proof
          (kernel_migrate_success_destination_runqueue_active
            sched
            src_cpu
            dst_cpu
            thread_id
            src
            dst
            index
            entity
            moved
            Hinv_full
            Hvalid
            Hneq
            Hsrc
            Hdst
            Hfind
            Hlookup
            Hstate
            Hspace
            Hdup
            Hmoved) as Hdestination_final.
        destruct Hfinal_rq as [_ Hfinal_lookup].
        destruct Hdestination_final as [_ Hdestination_lookup].
        rewrite Hfinal_lookup in Hdestination_lookup.
        inversion Hdestination_lookup.
        reflexivity.
      }
      subst final_rq.
      pose proof Hfinal_active as Happend_active.
      apply (proj1 (refresh_active_entity_iff _ _ _)) in Happend_active.
      assert (Hspace_lt : (er_entity_count dst < eevdf_max_entities)%nat)
        by (apply Nat.ltb_lt; exact Hspace).
      pose proof (append_entity_active_cases_shaped
        dst
        moved
        final_index
        running
        Hdst_shape
        Hspace_lt
        Happend_active) as Hcases.
      destruct Hcases as [[_Hnew_index Hrunning_moved] | [_Hold_index Hdst_old_active]].
      * subst running.
        pose proof (entity_as_migrated_runnable_state dst entity moved Hmoved)
          as Hmoved_state.
        rewrite Hmoved_state in Hrunning_state.
        discriminate.
      * destruct (Hrunning
          dst_cpu
          dst
          final_index
          running
          Hdst_active
          Hdst_old_active
          Hrunning_state) as [cpu [Hslot [Hhas [Hthread Hgen]]]].
        exists cpu.
        split.
        -- apply kernel_migrate_success_preserves_cpu_slot with
             (sched := sched)
             (src := src)
             (dst := dst)
             (index := index)
             (entity := entity)
             (moved := moved);
             auto.
        -- repeat split; auto.
    + assert (Horiginal_rq : kernel_active_runqueue sched entity_cpu final_rq).
      {
        destruct Hfinal_rq as [Hlt Hlookup_final].
        unfold kernel_migrate_runnable in Hlookup_final.
        rewrite Hvalid in Hlookup_final.
        rewrite Hneq in Hlookup_final.
        rewrite Hsrc in Hlookup_final.
        rewrite Hdst in Hlookup_final.
        rewrite Hfind in Hlookup_final.
        rewrite Hlookup in Hlookup_final.
        rewrite Hstate in Hlookup_final.
        rewrite Hspace in Hlookup_final.
        rewrite Hdup in Hlookup_final.
        rewrite Hmoved in Hlookup_final.
        simpl in Hlookup_final.
        unfold kernel_replace_runqueue in Hlookup_final.
        simpl in Hlookup_final.
        rewrite nth_error_replace_nth_neq in Hlookup_final by congruence.
        rewrite nth_error_replace_nth_neq in Hlookup_final by congruence.
        unfold kernel_active_runqueue.
        split.
        - unfold kernel_migrate_runnable in Hlt.
          rewrite Hvalid in Hlt.
          rewrite Hneq in Hlt.
          rewrite Hsrc in Hlt.
          rewrite Hdst in Hlt.
          rewrite Hfind in Hlt.
          rewrite Hlookup in Hlt.
          rewrite Hstate in Hlt.
          rewrite Hspace in Hlt.
          rewrite Hdup in Hlt.
          rewrite Hmoved in Hlt.
          simpl in Hlt.
          unfold kernel_replace_runqueue in Hlt.
          simpl in Hlt.
          exact Hlt.
        - exact Hlookup_final.
      }
      destruct (Hrunning
        entity_cpu
        final_rq
        final_index
        running
        Horiginal_rq
        Hfinal_active
        Hrunning_state) as [cpu [Hslot [Hhas [Hthread Hgen]]]].
      exists cpu.
      split.
      * apply kernel_migrate_success_preserves_cpu_slot with
          (sched := sched)
          (src := src)
          (dst := dst)
          (index := index)
          (entity := entity)
          (moved := moved);
          auto.
      * repeat split; auto.
Qed.

Theorem kernel_migrate_success_preserves_no_cross_cpu_current_duplicates :
  forall sched src_cpu dst_cpu thread_id src dst index entity moved,
    kernel_sched_invariant sched ->
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
    kernel_no_cross_cpu_current_duplicates
      (fst (kernel_migrate_runnable sched src_cpu dst_cpu thread_id)).
Proof.
  intros sched src_cpu dst_cpu thread_id src dst index entity moved
    Hinv Hvalid Hneq Hsrc Hdst Hfind Hlookup Hstate Hspace Hdup Hmoved.
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
  simpl.
  destruct Hinv as [_ [_ [_ [_ [_ [Hnodup _]]]]]].
  apply kernel_replace_runqueue_preserves_no_cross_cpu_current_duplicates.
  apply kernel_replace_runqueue_preserves_no_cross_cpu_current_duplicates.
  exact Hnodup.
Qed.

Theorem kernel_migrate_success_preserves_activation_targets :
  forall sched src_cpu dst_cpu thread_id src dst index entity moved,
    kernel_sched_invariant sched ->
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
    kernel_activation_targets_valid_cpus
      (fst (kernel_migrate_runnable sched src_cpu dst_cpu thread_id)).
Proof.
  intros sched src_cpu dst_cpu thread_id src dst index entity moved
    Hinv Hvalid Hneq Hsrc Hdst Hfind Hlookup Hstate Hspace Hdup Hmoved.
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
  simpl.
  destruct Hinv as [_ [_ [_ [_ [_ [_ Htargets]]]]]].
  apply kernel_replace_runqueue_preserves_activation_targets.
  apply kernel_replace_runqueue_preserves_activation_targets.
  exact Htargets.
Qed.

Theorem kernel_migrate_success_preserves_invariant :
  forall sched src_cpu dst_cpu thread_id src dst index entity moved,
    kernel_sched_invariant sched ->
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
    kernel_sched_invariant
      (fst (kernel_migrate_runnable sched src_cpu dst_cpu thread_id)).
Proof.
  intros sched src_cpu dst_cpu thread_id src dst index entity moved
    Hinv Hvalid Hneq Hsrc Hdst Hfind Hlookup Hstate Hspace Hdup Hmoved.
  split.
  - apply kernel_migrate_success_preserves_shape with
      (src := src)
      (dst := dst)
      (index := index)
      (entity := entity)
      (moved := moved); auto.
  - split.
    + apply kernel_migrate_success_preserves_runqueues_invariant with
        (src := src)
        (dst := dst)
        (index := index)
        (entity := entity)
        (moved := moved); auto.
    + split.
      * apply kernel_migrate_success_preserves_global_thread_ids_unique with
          (src := src)
          (dst := dst)
          (index := index)
          (entity := entity)
          (moved := moved); auto.
      * split.
        -- apply kernel_migrate_success_preserves_current_matches with
             (src := src)
             (dst := dst)
             (index := index)
             (entity := entity)
             (moved := moved); auto.
        -- split.
           ++ apply kernel_migrate_success_preserves_running_entity_has_current with
                (src := src)
                (dst := dst)
                (index := index)
                (entity := entity)
                (moved := moved); auto.
           ++ split.
              ** apply kernel_migrate_success_preserves_no_cross_cpu_current_duplicates with
                   (src := src)
                   (dst := dst)
                   (index := index)
                   (entity := entity)
                   (moved := moved); auto.
              ** apply kernel_migrate_success_preserves_activation_targets with
                   (src := src)
                   (dst := dst)
                   (index := index)
                   (entity := entity)
                   (moved := moved); auto.
Qed.

Theorem kernel_migrate_runnable_preserves_invariant :
  forall sched src_cpu dst_cpu thread_id,
    kernel_sched_invariant sched ->
    kernel_sched_invariant
      (fst (kernel_migrate_runnable sched src_cpu dst_cpu thread_id)).
Proof.
  intros sched src_cpu dst_cpu thread_id Hinv.
  destruct
    (orb
      (negb (kernel_valid_cpu sched src_cpu))
      (negb (kernel_valid_cpu sched dst_cpu))) eqn:Hvalid.
  - apply kernel_migrate_invalid_cpu_preserves_invariant; auto.
  - destruct (Nat.eqb src_cpu dst_cpu) eqn:Hneq.
    + unfold kernel_migrate_runnable.
      rewrite Hvalid.
      rewrite Hneq.
      simpl.
      exact Hinv.
    + destruct (kernel_lookup_runqueue sched src_cpu) as [src |] eqn:Hsrc.
      * destruct (kernel_lookup_runqueue sched dst_cpu) as [dst |] eqn:Hdst.
        -- destruct (find_entity_index src thread_id) as [index |] eqn:Hfind.
           ++ destruct (lookup_entity src index) as [entity |] eqn:Hlookup.
              ** destruct (ee_state entity) eqn:Hstate.
                 --- apply kernel_migrate_non_runnable_preserves_invariant
                       with (src := src) (dst := dst) (index := index)
                            (entity := entity); auto;
                       congruence.
                 --- destruct (er_entity_count dst <? eevdf_max_entities)%nat
                       eqn:Hspace.
                     +++ destruct (find_entity_index dst thread_id)
                           as [duplicate_index |] eqn:Hdup.
                         *** apply kernel_migrate_duplicate_destination_preserves_invariant
                               with
                                 (src := src)
                                 (dst := dst)
                                 (index := index)
                                 (entity := entity)
                                 (duplicate_index := duplicate_index);
                               auto.
                         *** destruct (entity_as_migrated_runnable dst entity)
                               as [moved |] eqn:Hmoved.
                             ---- apply kernel_migrate_success_preserves_invariant
                                    with
                                      (src := src)
                                      (dst := dst)
                                      (index := index)
                                      (entity := entity)
                                      (moved := moved);
                                    auto.
                             ---- apply kernel_migrate_refresh_overflow_preserves_invariant
                                    with
                                      (src := src)
                                      (dst := dst)
                                      (index := index)
                                      (entity := entity);
                                    auto.
                     +++ apply kernel_migrate_destination_full_preserves_invariant
                           with (src := src) (dst := dst) (index := index)
                                (entity := entity); auto.
                 --- apply kernel_migrate_non_runnable_preserves_invariant
                       with (src := src) (dst := dst) (index := index)
                            (entity := entity); auto;
                       congruence.
                 --- apply kernel_migrate_non_runnable_preserves_invariant
                       with (src := src) (dst := dst) (index := index)
                            (entity := entity); auto;
                       congruence.
                 --- apply kernel_migrate_non_runnable_preserves_invariant
                       with (src := src) (dst := dst) (index := index)
                            (entity := entity); auto;
                       congruence.
              ** unfold kernel_migrate_runnable.
                 rewrite Hvalid.
                 rewrite Hneq.
                 rewrite Hsrc.
                 rewrite Hdst.
                 rewrite Hfind.
                 rewrite Hlookup.
                 simpl.
                 exact Hinv.
           ++ apply kernel_migrate_missing_thread_preserves_invariant
                with (src := src) (dst := dst); auto.
        -- unfold kernel_migrate_runnable.
           rewrite Hvalid.
           rewrite Hneq.
           rewrite Hsrc.
           rewrite Hdst.
           simpl.
           exact Hinv.
      * destruct (kernel_lookup_runqueue sched dst_cpu) as [dst |] eqn:Hdst.
        -- unfold kernel_migrate_runnable.
           rewrite Hvalid.
           rewrite Hneq.
           rewrite Hsrc.
           simpl.
           exact Hinv.
        -- unfold kernel_migrate_runnable.
           rewrite Hvalid.
           rewrite Hneq.
           rewrite Hsrc.
           simpl.
           exact Hinv.
Qed.
