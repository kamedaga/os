From Stdlib Require Import Arith.PeanoNat Bool.Bool Lists.List.
From Pacha.Scheduling Require Import ProtocolModel ProtocolTransitions.

Definition status_cpu_valid (s : system_state) : Prop :=
  forall tid t cpu,
    lookup_thread s tid = Some t ->
    status_owner_cpu (ts_status t) = Some cpu ->
    valid_cpu s cpu.

Definition no_cpu_double_running (s : system_state) : Prop :=
  forall tid1 tid2 t1 t2 cpu,
    lookup_thread s tid1 = Some t1 ->
    lookup_thread s tid2 = Some t2 ->
    ts_status t1 = Running cpu ->
    ts_status t2 = Running cpu ->
    tid1 = tid2.

Definition no_cpu_double_pending (s : system_state) : Prop :=
  forall tid1 tid2 t1 t2 cpu,
    lookup_thread s tid1 = Some t1 ->
    lookup_thread s tid2 = Some t2 ->
    ts_status t1 = Pending cpu ->
    ts_status t2 = Pending cpu ->
    tid1 = tid2.

Definition no_cpu_running_pending_overlap (s : system_state) : Prop :=
  forall running_tid pending_tid running_t pending_t cpu,
    lookup_thread s running_tid = Some running_t ->
    lookup_thread s pending_tid = Some pending_t ->
    ts_status running_t = Running cpu ->
    ts_status pending_t = Pending cpu ->
    False.

Definition protocol_invariant (s : system_state) : Prop :=
  status_cpu_valid s /\
  no_cpu_double_running s /\
  no_cpu_double_pending s /\
  no_cpu_running_pending_overlap s.

Lemma lookup_replace_nth_eq :
  forall (A : Type) (items : list A) index item old_item,
    nth_error items index = Some old_item ->
    nth_error (replace_nth items index item) index = Some item.
Proof.
  intros A items.
  induction items as [| head rest IH]; intros index item old_item Hlookup;
    destruct index; simpl in *; try discriminate; auto.
  apply IH with (old_item := old_item).
  exact Hlookup.
Qed.

Lemma lookup_replace_nth_neq :
  forall (A : Type) (items : list A) replace_index lookup_index item,
    lookup_index <> replace_index ->
    nth_error (replace_nth items replace_index item) lookup_index =
      nth_error items lookup_index.
Proof.
  intros A items.
  induction items as [| head rest IH];
    intros replace_index lookup_index item Hneq;
    destruct replace_index, lookup_index; simpl in *; try reflexivity.
  - contradiction.
  - apply IH. congruence.
Qed.

Lemma lookup_replace_thread_eq :
  forall s tid old_thread new_thread,
    lookup_thread s tid = Some old_thread ->
    lookup_thread (replace_thread s tid new_thread) tid = Some new_thread.
Proof.
  intros s tid old_thread new_thread Hlookup.
  unfold lookup_thread, replace_thread in *.
  simpl.
  apply lookup_replace_nth_eq with (old_item := old_thread).
  exact Hlookup.
Qed.

Lemma lookup_replace_thread_neq :
  forall s replaced_tid lookup_tid new_thread,
    lookup_tid <> replaced_tid ->
    lookup_thread (replace_thread s replaced_tid new_thread) lookup_tid =
      lookup_thread s lookup_tid.
Proof.
  intros s replaced_tid lookup_tid new_thread Hneq.
  unfold lookup_thread, replace_thread.
  simpl.
  apply lookup_replace_nth_neq.
  exact Hneq.
Qed.

Lemma status_owns_cpu_owner :
  forall status cpu,
    status_owner_cpu status = Some cpu ->
    status_owns_cpu status cpu = true.
Proof.
  intros status cpu Howner.
  destruct status; simpl in *; try discriminate.
  - inversion Howner; subst. apply Nat.eqb_refl.
  - inversion Howner; subst. apply Nat.eqb_refl.
Qed.

Lemma cpu_has_owner_false_lookup :
  forall s cpu tid t,
    cpu_has_owner s cpu = false ->
    lookup_thread s tid = Some t ->
    status_owner_cpu (ts_status t) = Some cpu ->
    False.
Proof.
  intros s cpu.
  unfold cpu_has_owner, lookup_thread.
  induction (threads s) as [| head rest IH];
    intros tid t Hownerless Hlookup Hstatus;
    destruct tid; simpl in *; try discriminate.
  - inversion Hlookup; subst.
    apply orb_false_iff in Hownerless as [Hhead _].
    rewrite (status_owns_cpu_owner (ts_status t) cpu Hstatus) in Hhead.
    discriminate.
  - apply orb_false_iff in Hownerless as [_ Hrest].
    eapply IH; eauto.
Qed.

Lemma status_cpu_valid_replace :
  forall s tid old_thread new_thread,
    lookup_thread s tid = Some old_thread ->
    status_cpu_valid s ->
    (forall cpu,
      status_owner_cpu (ts_status new_thread) = Some cpu ->
      valid_cpu s cpu) ->
    status_cpu_valid (replace_thread s tid new_thread).
Proof.
  intros s tid old_thread new_thread Hlookup Hvalid Hnew.
  unfold status_cpu_valid in *.
  intros lookup_tid t cpu Hlookup' Howner.
  destruct (Nat.eq_dec lookup_tid tid) as [Heq | Hneq].
  - subst.
    rewrite (lookup_replace_thread_eq s tid old_thread new_thread Hlookup) in Hlookup'.
    inversion Hlookup'; subst.
    exact (Hnew cpu Howner).
  - rewrite lookup_replace_thread_neq in Hlookup' by exact Hneq.
    exact (Hvalid lookup_tid t cpu Hlookup' Howner).
Qed.

Lemma no_cpu_double_running_replace_ownerless :
  forall s tid old_thread new_thread,
    lookup_thread s tid = Some old_thread ->
    no_cpu_double_running s ->
    (forall cpu, ts_status new_thread <> Running cpu) ->
    no_cpu_double_running (replace_thread s tid new_thread).
Proof.
  intros s tid old_thread new_thread Hlookup Hnodup Hnot_running.
  unfold no_cpu_double_running in *.
  intros tid1 tid2 t1 t2 cpu Hlookup1 Hlookup2 Hrun1 Hrun2.
  destruct (Nat.eq_dec tid1 tid) as [Htid1 | Htid1];
    destruct (Nat.eq_dec tid2 tid) as [Htid2 | Htid2].
  - subst. reflexivity.
  - subst.
    rewrite (lookup_replace_thread_eq s tid old_thread new_thread Hlookup) in Hlookup1.
    inversion Hlookup1; subst.
    contradiction (Hnot_running cpu Hrun1).
  - subst.
    rewrite (lookup_replace_thread_eq s tid old_thread new_thread Hlookup) in Hlookup2.
    inversion Hlookup2; subst.
    contradiction (Hnot_running cpu Hrun2).
  - rewrite lookup_replace_thread_neq in Hlookup1 by exact Htid1.
    rewrite lookup_replace_thread_neq in Hlookup2 by exact Htid2.
    exact (Hnodup tid1 tid2 t1 t2 cpu Hlookup1 Hlookup2 Hrun1 Hrun2).
Qed.

Lemma no_cpu_double_pending_replace_ownerless :
  forall s tid old_thread new_thread,
    lookup_thread s tid = Some old_thread ->
    no_cpu_double_pending s ->
    (forall cpu, ts_status new_thread <> Pending cpu) ->
    no_cpu_double_pending (replace_thread s tid new_thread).
Proof.
  intros s tid old_thread new_thread Hlookup Hnodup Hnot_pending.
  unfold no_cpu_double_pending in *.
  intros tid1 tid2 t1 t2 cpu Hlookup1 Hlookup2 Hpending1 Hpending2.
  destruct (Nat.eq_dec tid1 tid) as [Htid1 | Htid1];
    destruct (Nat.eq_dec tid2 tid) as [Htid2 | Htid2].
  - subst. reflexivity.
  - subst.
    rewrite (lookup_replace_thread_eq s tid old_thread new_thread Hlookup) in Hlookup1.
    inversion Hlookup1; subst.
    contradiction (Hnot_pending cpu Hpending1).
  - subst.
    rewrite (lookup_replace_thread_eq s tid old_thread new_thread Hlookup) in Hlookup2.
    inversion Hlookup2; subst.
    contradiction (Hnot_pending cpu Hpending2).
  - rewrite lookup_replace_thread_neq in Hlookup1 by exact Htid1.
    rewrite lookup_replace_thread_neq in Hlookup2 by exact Htid2.
    exact (Hnodup tid1 tid2 t1 t2 cpu Hlookup1 Hlookup2 Hpending1 Hpending2).
Qed.

Lemma no_cpu_running_pending_overlap_replace_ownerless :
  forall s tid old_thread new_thread,
    lookup_thread s tid = Some old_thread ->
    no_cpu_running_pending_overlap s ->
    (forall cpu, ts_status new_thread <> Running cpu) ->
    (forall cpu, ts_status new_thread <> Pending cpu) ->
    no_cpu_running_pending_overlap (replace_thread s tid new_thread).
Proof.
  intros s tid old_thread new_thread Hlookup Hoverlap Hnot_running Hnot_pending.
  unfold no_cpu_running_pending_overlap in *.
  intros running_tid pending_tid running_t pending_t cpu
    Hrunning_lookup Hpending_lookup Hrunning Hpending.
  destruct (Nat.eq_dec running_tid tid) as [Hrunning_tid | Hrunning_tid].
  - subst.
    rewrite (lookup_replace_thread_eq s tid old_thread new_thread Hlookup)
      in Hrunning_lookup.
    inversion Hrunning_lookup; subst.
    contradiction (Hnot_running cpu Hrunning).
  - destruct (Nat.eq_dec pending_tid tid) as [Hpending_tid | Hpending_tid].
    + subst.
      rewrite (lookup_replace_thread_eq s tid old_thread new_thread Hlookup)
        in Hpending_lookup.
      inversion Hpending_lookup; subst.
      contradiction (Hnot_pending cpu Hpending).
    + rewrite lookup_replace_thread_neq in Hrunning_lookup by exact Hrunning_tid.
      rewrite lookup_replace_thread_neq in Hpending_lookup by exact Hpending_tid.
      exact (Hoverlap running_tid pending_tid running_t pending_t cpu
        Hrunning_lookup Hpending_lookup Hrunning Hpending).
Qed.

Lemma replace_ownerless_preserves_protocol_invariant :
  forall s tid old_thread new_thread,
    lookup_thread s tid = Some old_thread ->
    protocol_invariant s ->
    (forall cpu, status_owner_cpu (ts_status new_thread) <> Some cpu) ->
    protocol_invariant (replace_thread s tid new_thread).
Proof.
  intros s tid old_thread new_thread Hlookup
    [Hvalid [Hrunning [Hpending Hoverlap]]] Hownerless.
  repeat split.
  - eapply status_cpu_valid_replace; eauto.
    intros cpu Howner.
    contradiction (Hownerless cpu Howner).
  - eapply no_cpu_double_running_replace_ownerless; eauto.
    intros cpu Hrunning_new.
    apply (Hownerless cpu).
    rewrite Hrunning_new.
    reflexivity.
  - eapply no_cpu_double_pending_replace_ownerless; eauto.
    intros cpu Hpending_new.
    apply (Hownerless cpu).
    rewrite Hpending_new.
    reflexivity.
  - eapply no_cpu_running_pending_overlap_replace_ownerless; eauto;
      intros cpu Hstatus;
      apply (Hownerless cpu);
      rewrite Hstatus;
      reflexivity.
Qed.

Lemma replace_with_pending_preserves_protocol_invariant :
  forall s tid old_thread cpu,
    lookup_thread s tid = Some old_thread ->
    protocol_invariant s ->
    valid_cpu s cpu ->
    cpu_has_owner s cpu = false ->
    protocol_invariant (replace_thread s tid (set_status old_thread (Pending cpu))).
Proof.
  intros s tid old_thread cpu Hlookup
    [Hvalid [Hrunning [Hpending Hoverlap]]] Hcpu_valid Hcpu_ownerless.
  repeat split.
  - eapply status_cpu_valid_replace; eauto.
    intros owner Howner.
    simpl in Howner.
    inversion Howner; subst.
    exact Hcpu_valid.
  - eapply no_cpu_double_running_replace_ownerless; eauto.
    intros owner Hrunning_new.
    discriminate Hrunning_new.
  - unfold no_cpu_double_pending in *.
    intros tid1 tid2 t1 t2 owner Hlookup1 Hlookup2 Hpending1 Hpending2.
    destruct (Nat.eq_dec tid1 tid) as [Htid1 | Htid1];
      destruct (Nat.eq_dec tid2 tid) as [Htid2 | Htid2].
    + subst. reflexivity.
    + subst.
      rewrite (lookup_replace_thread_eq s tid old_thread
        (set_status old_thread (Pending cpu)) Hlookup) in Hlookup1.
      inversion Hlookup1; subst.
      simpl in Hpending1.
      inversion Hpending1; subst.
      rewrite lookup_replace_thread_neq in Hlookup2 by exact Htid2.
      exfalso.
      eapply cpu_has_owner_false_lookup; eauto.
      rewrite Hpending2.
      reflexivity.
    + subst.
      rewrite (lookup_replace_thread_eq s tid old_thread
        (set_status old_thread (Pending cpu)) Hlookup) in Hlookup2.
      inversion Hlookup2; subst.
      simpl in Hpending2.
      inversion Hpending2; subst.
      rewrite lookup_replace_thread_neq in Hlookup1 by exact Htid1.
      exfalso.
      eapply cpu_has_owner_false_lookup; eauto.
      rewrite Hpending1.
      reflexivity.
    + rewrite lookup_replace_thread_neq in Hlookup1 by exact Htid1.
      rewrite lookup_replace_thread_neq in Hlookup2 by exact Htid2.
      exact (Hpending tid1 tid2 t1 t2 owner Hlookup1 Hlookup2 Hpending1 Hpending2).
  - unfold no_cpu_running_pending_overlap in *.
    intros running_tid pending_tid running_t pending_t owner
      Hrunning_lookup Hpending_lookup Hrunning_status Hpending_status.
    destruct (Nat.eq_dec running_tid tid) as [Hrunning_tid | Hrunning_tid].
    + subst.
      rewrite (lookup_replace_thread_eq s tid old_thread
        (set_status old_thread (Pending cpu)) Hlookup) in Hrunning_lookup.
      inversion Hrunning_lookup; subst.
      simpl in Hrunning_status.
      discriminate Hrunning_status.
    + destruct (Nat.eq_dec pending_tid tid) as [Hpending_tid | Hpending_tid].
      * subst.
        rewrite (lookup_replace_thread_eq s tid old_thread
          (set_status old_thread (Pending cpu)) Hlookup) in Hpending_lookup.
        inversion Hpending_lookup; subst.
        simpl in Hpending_status.
        inversion Hpending_status; subst.
        rewrite lookup_replace_thread_neq in Hrunning_lookup by exact Hrunning_tid.
        eapply cpu_has_owner_false_lookup; eauto.
        rewrite Hrunning_status.
        reflexivity.
      * rewrite lookup_replace_thread_neq in Hrunning_lookup by exact Hrunning_tid.
        rewrite lookup_replace_thread_neq in Hpending_lookup by exact Hpending_tid.
        exact (Hoverlap running_tid pending_tid running_t pending_t owner
          Hrunning_lookup Hpending_lookup Hrunning_status Hpending_status).
Qed.

Lemma replace_pending_with_running_preserves_protocol_invariant :
  forall s tid old_thread cpu,
    lookup_thread s tid = Some old_thread ->
    ts_status old_thread = Pending cpu ->
    protocol_invariant s ->
    protocol_invariant (replace_thread s tid (set_status old_thread (Running cpu))).
Proof.
  intros s tid old_thread cpu Hlookup Hold_pending
    [Hvalid [Hrunning [Hpending Hoverlap]]].
  repeat split.
  - eapply status_cpu_valid_replace; eauto.
    intros owner Howner.
    simpl in Howner.
    inversion Howner; subst.
    eapply Hvalid; eauto.
    rewrite Hold_pending.
    reflexivity.
  - unfold no_cpu_double_running in *.
    intros tid1 tid2 t1 t2 owner Hlookup1 Hlookup2 Hrunning1 Hrunning2.
    destruct (Nat.eq_dec tid1 tid) as [Htid1 | Htid1];
      destruct (Nat.eq_dec tid2 tid) as [Htid2 | Htid2].
    + subst. reflexivity.
    + subst.
      rewrite (lookup_replace_thread_eq s tid old_thread
        (set_status old_thread (Running cpu)) Hlookup) in Hlookup1.
      inversion Hlookup1; subst.
      simpl in Hrunning1.
      inversion Hrunning1; subst.
      rewrite lookup_replace_thread_neq in Hlookup2 by exact Htid2.
      exfalso.
      exact (Hoverlap tid2 tid t2 old_thread owner
        Hlookup2 Hlookup Hrunning2 Hold_pending).
    + subst.
      rewrite (lookup_replace_thread_eq s tid old_thread
        (set_status old_thread (Running cpu)) Hlookup) in Hlookup2.
      inversion Hlookup2; subst.
      simpl in Hrunning2.
      inversion Hrunning2; subst.
      rewrite lookup_replace_thread_neq in Hlookup1 by exact Htid1.
      exfalso.
      exact (Hoverlap tid1 tid t1 old_thread owner
        Hlookup1 Hlookup Hrunning1 Hold_pending).
    + rewrite lookup_replace_thread_neq in Hlookup1 by exact Htid1.
      rewrite lookup_replace_thread_neq in Hlookup2 by exact Htid2.
      exact (Hrunning tid1 tid2 t1 t2 owner Hlookup1 Hlookup2 Hrunning1 Hrunning2).
  - eapply no_cpu_double_pending_replace_ownerless; eauto.
    intros owner Hpending_new.
    discriminate Hpending_new.
  - unfold no_cpu_running_pending_overlap in *.
    intros running_tid pending_tid running_t pending_t owner
      Hrunning_lookup Hpending_lookup Hrunning_status Hpending_status.
    destruct (Nat.eq_dec running_tid tid) as [Hrunning_tid | Hrunning_tid].
    + subst.
      rewrite (lookup_replace_thread_eq s tid old_thread
        (set_status old_thread (Running cpu)) Hlookup) in Hrunning_lookup.
      inversion Hrunning_lookup; subst.
      simpl in Hrunning_status.
      inversion Hrunning_status; subst.
      destruct (Nat.eq_dec pending_tid tid) as [Hpending_tid | Hpending_tid].
      * subst.
        rewrite (lookup_replace_thread_eq s tid old_thread
          (set_status old_thread (Running owner)) Hlookup) in Hpending_lookup.
        inversion Hpending_lookup; subst.
        simpl in Hpending_status.
        discriminate Hpending_status.
      * rewrite lookup_replace_thread_neq in Hpending_lookup by exact Hpending_tid.
        pose proof (Hpending tid pending_tid old_thread pending_t owner
          Hlookup Hpending_lookup Hold_pending Hpending_status) as Heq.
        symmetry in Heq.
        contradiction.
    + destruct (Nat.eq_dec pending_tid tid) as [Hpending_tid | Hpending_tid].
      * subst.
        rewrite (lookup_replace_thread_eq s tid old_thread
          (set_status old_thread (Running cpu)) Hlookup) in Hpending_lookup.
        inversion Hpending_lookup; subst.
        simpl in Hpending_status.
        discriminate Hpending_status.
      * rewrite lookup_replace_thread_neq in Hrunning_lookup by exact Hrunning_tid.
        rewrite lookup_replace_thread_neq in Hpending_lookup by exact Hpending_tid.
        exact (Hoverlap running_tid pending_tid running_t pending_t owner
          Hrunning_lookup Hpending_lookup Hrunning_status Hpending_status).
Qed.

Theorem commit_preserves_protocol_invariant :
  forall s s' cpu tid gen,
    protocol_invariant s ->
    commit s cpu tid gen = Some s' ->
    protocol_invariant s'.
Proof.
  intros s s' cpu tid gen Hinv Hcommit.
  unfold commit in Hcommit.
  destruct (Nat.ltb cpu (cpu_count s)) eqn:Hcpu_valid; try discriminate.
  destruct (negb (cpu_has_owner s cpu)) eqn:Hcpu_ownerless; try discriminate.
  destruct (lookup_thread s tid) as [t |] eqn:Hlookup; try discriminate.
  destruct (generation_matches t gen) eqn:Hgen; try discriminate.
  destruct (ts_status t) eqn:Hstatus; try discriminate.
  inversion Hcommit; subst.
  apply replace_with_pending_preserves_protocol_invariant; auto.
  - unfold valid_cpu.
    apply Nat.ltb_lt.
    exact Hcpu_valid.
  - apply negb_true_iff in Hcpu_ownerless.
    exact Hcpu_ownerless.
Qed.

Theorem claim_preserves_protocol_invariant :
  forall s s' cpu tid gen,
    protocol_invariant s ->
    claim s cpu tid gen = Some s' ->
    protocol_invariant s'.
Proof.
  intros s s' cpu tid gen Hinv Hclaim.
  unfold claim in Hclaim.
  destruct (lookup_thread s tid) as [t |] eqn:Hlookup; try discriminate.
  destruct (generation_matches t gen) eqn:Hgen; try discriminate.
  destruct (ts_status t) as [| | running_cpu | pending_cpu | |] eqn:Hstatus;
    try discriminate.
  destruct (Nat.eqb pending_cpu cpu) eqn:Hcpu; try discriminate.
  inversion Hclaim; subst.
  apply Nat.eqb_eq in Hcpu.
  subst pending_cpu.
  eapply replace_pending_with_running_preserves_protocol_invariant; eauto.
Qed.

Theorem preempt_preserves_protocol_invariant :
  forall s s' cpu tid gen,
    protocol_invariant s ->
    preempt s cpu tid gen = Some s' ->
    protocol_invariant s'.
Proof.
  intros s s' cpu tid gen Hinv Hpreempt.
  unfold preempt in Hpreempt.
  destruct (lookup_thread s tid) as [t |] eqn:Hlookup; try discriminate.
  destruct (generation_matches t gen) eqn:Hgen; try discriminate.
  destruct (ts_status t) as [| | running_cpu | pending_cpu | |] eqn:Hstatus;
    try discriminate.
  destruct (Nat.eqb running_cpu cpu) eqn:Hcpu; try discriminate.
  inversion Hpreempt; subst.
  eapply replace_ownerless_preserves_protocol_invariant; eauto.
  intros owner Howner.
  discriminate Howner.
Qed.

Theorem block_preserves_protocol_invariant :
  forall s s' cpu tid gen,
    protocol_invariant s ->
    block s cpu tid gen = Some s' ->
    protocol_invariant s'.
Proof.
  intros s s' cpu tid gen Hinv Hblock.
  unfold block in Hblock.
  destruct (lookup_thread s tid) as [t |] eqn:Hlookup; try discriminate.
  destruct (generation_matches t gen) eqn:Hgen; try discriminate.
  destruct (ts_status t) as [| | running_cpu | pending_cpu | |] eqn:Hstatus;
    try discriminate.
  destruct (Nat.eqb running_cpu cpu) eqn:Hcpu; try discriminate.
  inversion Hblock; subst.
  eapply replace_ownerless_preserves_protocol_invariant; eauto.
  intros owner Howner.
  discriminate Howner.
Qed.

Theorem wake_preserves_protocol_invariant :
  forall s s' tid gen,
    protocol_invariant s ->
    wake s tid gen = Some s' ->
    protocol_invariant s'.
Proof.
  intros s s' tid gen Hinv Hwake.
  unfold wake in Hwake.
  destruct (lookup_thread s tid) as [t |] eqn:Hlookup; try discriminate.
  destruct (generation_matches t gen) eqn:Hgen; try discriminate.
  destruct (ts_status t) eqn:Hstatus; try discriminate.
  inversion Hwake; subst.
  eapply replace_ownerless_preserves_protocol_invariant; eauto.
  intros owner Howner.
  discriminate Howner.
Qed.

Theorem exit_thread_preserves_protocol_invariant :
  forall s s' tid gen,
    protocol_invariant s ->
    exit_thread s tid gen = Some s' ->
    protocol_invariant s'.
Proof.
  intros s s' tid gen Hinv Hexit.
  unfold exit_thread in Hexit.
  destruct (lookup_thread s tid) as [t |] eqn:Hlookup; try discriminate.
  destruct (generation_matches t gen) eqn:Hgen; try discriminate.
  destruct (is_live_status (ts_status t)) eqn:Hlive; try discriminate.
  inversion Hexit; subst.
  eapply replace_ownerless_preserves_protocol_invariant; eauto.
  intros owner Howner.
  discriminate Howner.
Qed.
