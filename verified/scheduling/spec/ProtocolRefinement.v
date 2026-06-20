From Stdlib Require Import Arith.PeanoNat Bool.Bool Lia Lists.List.
From Pacha.Scheduling Require Import
  ProtocolModel
  ProtocolTransitions
  ProtocolInvariants
  ProtocolConcrete.

Import ListNotations.

Definition abstract_of_concrete
    (s : concrete_state)
  : system_state :=
  {|
    cpu_count := cs_cpu_count s;
    threads := map concrete_thread_to_abstract (cs_threads s);
  |}.

Definition concrete_shape
    (s : concrete_state)
  : Prop :=
  cs_cpu_count s <= max_cpus /\
  cs_thread_count s <= max_threads /\
  length (cs_threads s) = max_threads /\
  length (cs_cpus s) = max_cpus.

Definition cpu_table_matches_threads
    (s : concrete_state)
  : Prop :=
  cs_cpus s = computed_cpus s.

Definition inactive_threads_empty
    (s : concrete_state)
  : Prop :=
  forall tid thread,
    concrete_lookup_thread_raw s tid = Some thread ->
    cs_thread_count s <= tid ->
    ct_status thread = CEmpty.

Definition concrete_invariant
    (s : concrete_state)
  : Prop :=
  concrete_shape s /\
  protocol_invariant (abstract_of_concrete s) /\
  inactive_threads_empty s /\
  cpu_table_matches_threads s.

Lemma replace_nth_length :
  forall (A : Type) (items : list A) index item,
    length (replace_nth items index item) = length items.
Proof.
  intros A items.
  induction items as [| head rest IH]; intros index item;
    destruct index; simpl; auto.
Qed.

Lemma map_replace_nth :
  forall (A B : Type) (f : A -> B) items index item,
    map f (replace_nth items index item) =
      replace_nth (map f items) index (f item).
Proof.
  intros A B f items.
  induction items as [| head rest IH]; intros index item;
    destruct index; simpl; auto.
  rewrite IH.
  reflexivity.
Qed.

Lemma abstract_sync_cpu_table :
  forall s,
    abstract_of_concrete (sync_cpu_table s) = abstract_of_concrete s.
Proof.
  intros s.
  reflexivity.
Qed.

Lemma cpu_table_matches_sync :
  forall s,
    cpu_table_matches_threads (sync_cpu_table s).
Proof.
  intros s.
  unfold cpu_table_matches_threads, sync_cpu_table, computed_cpus.
  simpl.
  reflexivity.
Qed.

Lemma concrete_shape_replace_thread :
  forall s tid thread,
    concrete_shape s ->
    concrete_shape (replace_concrete_thread s tid thread).
Proof.
  intros s tid thread [Hcpu [Hthread [Hthreads Hcpus]]].
  repeat split; simpl; auto.
  rewrite replace_nth_length.
  exact Hthreads.
Qed.

Lemma concrete_shape_sync :
  forall s,
    cs_cpu_count s <= max_cpus ->
    cs_thread_count s <= max_threads ->
    length (cs_threads s) = max_threads ->
    concrete_shape (sync_cpu_table s).
Proof.
  intros s Hcpu Hthread Hthreads.
  split.
  - simpl. exact Hcpu.
  - split.
    + simpl. exact Hthread.
    + split.
      * simpl. exact Hthreads.
      * simpl.
        reflexivity.
Qed.

Lemma concrete_shape_sync_replace_thread :
  forall s tid thread,
    concrete_shape s ->
    concrete_shape (sync_cpu_table (replace_concrete_thread s tid thread)).
Proof.
  intros s tid thread Hshape.
  destruct Hshape as [Hcpu [Hthread [Hthreads Hcpus]]].
  apply concrete_shape_sync; simpl; auto.
  rewrite replace_nth_length.
  exact Hthreads.
Qed.

Lemma inactive_threads_empty_replace_active :
  forall s tid new_thread,
    tid < cs_thread_count s ->
    inactive_threads_empty s ->
    inactive_threads_empty (replace_concrete_thread s tid new_thread).
Proof.
  intros s tid new_thread Hactive Hinactive.
  unfold inactive_threads_empty in *.
  intros lookup_tid thread Hlookup Htail.
  unfold concrete_lookup_thread_raw, replace_concrete_thread in Hlookup.
  simpl in Hlookup.
  simpl in Htail.
  destruct (Nat.eq_dec lookup_tid tid) as [Heq | Hneq].
  - subst lookup_tid.
    lia.
  - rewrite lookup_replace_nth_neq in Hlookup by exact Hneq.
    eapply Hinactive; eauto.
Qed.

Lemma inactive_threads_empty_sync_replace_active :
  forall s tid new_thread,
    tid < cs_thread_count s ->
    inactive_threads_empty s ->
    inactive_threads_empty (sync_cpu_table (replace_concrete_thread s tid new_thread)).
Proof.
  intros s tid new_thread Hactive Hinactive.
  apply inactive_threads_empty_replace_active; auto.
Qed.

Lemma c_status_owns_cpu_refines :
  forall thread cpu,
    c_status_owns_cpu thread cpu =
      status_owns_cpu (ts_status (concrete_thread_to_abstract thread)) cpu.
Proof.
  intros thread cpu.
  destruct thread as [gen status owner].
  destruct status; simpl; reflexivity.
Qed.

Lemma concrete_cpu_has_owner_refines :
  forall s cpu,
    concrete_cpu_has_owner s cpu =
      cpu_has_owner (abstract_of_concrete s) cpu.
Proof.
  intros s cpu.
  unfold concrete_cpu_has_owner, cpu_has_owner, abstract_of_concrete.
  simpl.
  induction (cs_threads s) as [| thread rest IH]; simpl; auto.
  rewrite c_status_owns_cpu_refines.
  rewrite IH.
  reflexivity.
Qed.

Lemma concrete_lookup_thread_refines :
  forall s tid thread,
    concrete_lookup_thread s tid = Some thread ->
    lookup_thread (abstract_of_concrete s) tid =
      Some (concrete_thread_to_abstract thread).
Proof.
  intros s tid thread Hlookup.
  unfold concrete_lookup_thread in Hlookup.
  destruct (concrete_valid_thread s tid) eqn:Hvalid; try discriminate.
  unfold concrete_lookup_thread_raw in Hlookup.
  unfold lookup_thread, abstract_of_concrete.
  simpl.
  rewrite nth_error_map.
  rewrite Hlookup.
  reflexivity.
Qed.

Lemma concrete_lookup_thread_raw_refines :
  forall s tid thread,
    concrete_lookup_thread_raw s tid = Some thread ->
    lookup_thread (abstract_of_concrete s) tid =
      Some (concrete_thread_to_abstract thread).
Proof.
  intros s tid thread Hlookup.
  unfold concrete_lookup_thread_raw in Hlookup.
  unfold lookup_thread, abstract_of_concrete.
  simpl.
  rewrite nth_error_map.
  rewrite Hlookup.
  reflexivity.
Qed.

Lemma concrete_lookup_thread_parts :
  forall s tid thread,
    concrete_lookup_thread s tid = Some thread ->
    concrete_valid_thread s tid = true /\
    concrete_lookup_thread_raw s tid = Some thread.
Proof.
  intros s tid thread Hlookup.
  unfold concrete_lookup_thread in Hlookup.
  destruct (concrete_valid_thread s tid) eqn:Hvalid; try discriminate.
  split; auto.
Qed.

Lemma concrete_lookup_thread_tid_lt_count :
  forall s tid thread,
    concrete_lookup_thread s tid = Some thread ->
    tid < cs_thread_count s.
Proof.
  intros s tid thread Hlookup.
  apply concrete_lookup_thread_parts in Hlookup as [Hvalid _].
  unfold concrete_valid_thread in Hvalid.
  apply Nat.ltb_lt.
  exact Hvalid.
Qed.

Lemma abstract_replace_concrete_thread :
  forall s tid thread,
    abstract_of_concrete (replace_concrete_thread s tid thread) =
      replace_thread
        (abstract_of_concrete s)
        tid
        (concrete_thread_to_abstract thread).
Proof.
  intros s tid thread.
  unfold abstract_of_concrete, replace_concrete_thread, replace_thread.
  simpl.
  rewrite map_replace_nth.
  reflexivity.
Qed.

Lemma abstract_sync_replace_concrete_thread :
  forall s tid thread,
    abstract_of_concrete (sync_cpu_table (replace_concrete_thread s tid thread)) =
      replace_thread
        (abstract_of_concrete s)
        tid
        (concrete_thread_to_abstract thread).
Proof.
  intros s tid thread.
  rewrite abstract_sync_cpu_table.
  apply abstract_replace_concrete_thread.
Qed.

Lemma sched_commit_refines_abstract :
  forall s cpu tid gen s',
    sched_commit s cpu tid gen =
      {|
        sr_rc := SchedOk;
        sr_state := s';
      |} ->
    commit (abstract_of_concrete s) cpu tid gen =
      Some (abstract_of_concrete s').
Proof.
  intros s cpu tid gen s' Hstep.
  unfold sched_commit in Hstep.
  destruct (andb (concrete_valid_cpu s cpu) (concrete_valid_thread s tid))
    eqn:Hbounds; try discriminate.
  apply andb_true_iff in Hbounds as [Hcpu Htid].
  unfold concrete_valid_cpu in Hcpu.
  destruct (negb (concrete_cpu_has_owner s cpu)) eqn:Hownerless; try discriminate.
  destruct (concrete_lookup_thread s tid) as [thread |] eqn:Hlookup; try discriminate.
  destruct (Nat.eqb (ct_generation thread) gen) eqn:Hgen; try discriminate.
  destruct (ct_status thread) eqn:Hstatus; try discriminate.
  inversion Hstep; subst; clear Hstep.
  unfold commit.
  simpl.
  rewrite Hcpu.
  rewrite <- concrete_cpu_has_owner_refines.
  rewrite Hownerless.
  rewrite (concrete_lookup_thread_refines s tid thread Hlookup).
  unfold generation_matches.
  simpl.
  rewrite Hgen.
  rewrite Hstatus.
  rewrite abstract_sync_replace_concrete_thread.
  reflexivity.
Qed.

Lemma sched_claim_refines_abstract :
  forall s cpu tid gen s',
    sched_claim s cpu tid gen =
      {|
        sr_rc := SchedOk;
        sr_state := s';
      |} ->
    claim (abstract_of_concrete s) cpu tid gen =
      Some (abstract_of_concrete s').
Proof.
  intros s cpu tid gen s' Hstep.
  unfold sched_claim in Hstep.
  destruct (andb (concrete_valid_cpu s cpu) (concrete_valid_thread s tid))
    eqn:Hbounds; try discriminate.
  destruct (concrete_lookup_thread s tid) as [thread |] eqn:Hlookup; try discriminate.
  destruct (Nat.eqb (ct_generation thread) gen) eqn:Hgen; try discriminate.
  destruct (ct_status thread) eqn:Hstatus; try discriminate.
  destruct (Nat.eqb (ct_cpu thread) cpu) eqn:Hcpu_match; try discriminate.
  inversion Hstep; subst; clear Hstep.
  unfold claim.
  rewrite (concrete_lookup_thread_refines s tid thread Hlookup).
  unfold generation_matches.
  simpl.
  rewrite Hgen.
  rewrite Hstatus.
  rewrite Hcpu_match.
  rewrite abstract_sync_replace_concrete_thread.
  reflexivity.
Qed.

Lemma sched_preempt_refines_abstract :
  forall s cpu tid gen s',
    sched_preempt s cpu tid gen =
      {|
        sr_rc := SchedOk;
        sr_state := s';
      |} ->
    preempt (abstract_of_concrete s) cpu tid gen =
      Some (abstract_of_concrete s').
Proof.
  intros s cpu tid gen s' Hstep.
  unfold sched_preempt in Hstep.
  destruct (andb (concrete_valid_cpu s cpu) (concrete_valid_thread s tid))
    eqn:Hbounds; try discriminate.
  destruct (concrete_lookup_thread s tid) as [thread |] eqn:Hlookup; try discriminate.
  destruct (Nat.eqb (ct_generation thread) gen) eqn:Hgen; try discriminate.
  destruct (ct_status thread) eqn:Hstatus; try discriminate.
  destruct (Nat.eqb (ct_cpu thread) cpu) eqn:Hcpu_match; try discriminate.
  inversion Hstep; subst; clear Hstep.
  unfold preempt.
  rewrite (concrete_lookup_thread_refines s tid thread Hlookup).
  unfold generation_matches.
  simpl.
  rewrite Hgen.
  rewrite Hstatus.
  rewrite Hcpu_match.
  rewrite abstract_sync_replace_concrete_thread.
  reflexivity.
Qed.

Lemma sched_block_refines_abstract :
  forall s cpu tid gen s',
    sched_block s cpu tid gen =
      {|
        sr_rc := SchedOk;
        sr_state := s';
      |} ->
    block (abstract_of_concrete s) cpu tid gen =
      Some (abstract_of_concrete s').
Proof.
  intros s cpu tid gen s' Hstep.
  unfold sched_block in Hstep.
  destruct (andb (concrete_valid_cpu s cpu) (concrete_valid_thread s tid))
    eqn:Hbounds; try discriminate.
  destruct (concrete_lookup_thread s tid) as [thread |] eqn:Hlookup; try discriminate.
  destruct (Nat.eqb (ct_generation thread) gen) eqn:Hgen; try discriminate.
  destruct (ct_status thread) eqn:Hstatus; try discriminate.
  destruct (Nat.eqb (ct_cpu thread) cpu) eqn:Hcpu_match; try discriminate.
  inversion Hstep; subst; clear Hstep.
  unfold block.
  rewrite (concrete_lookup_thread_refines s tid thread Hlookup).
  unfold generation_matches.
  simpl.
  rewrite Hgen.
  rewrite Hstatus.
  rewrite Hcpu_match.
  rewrite abstract_sync_replace_concrete_thread.
  reflexivity.
Qed.

Lemma sched_wake_refines_abstract :
  forall s tid gen s',
    sched_wake s tid gen =
      {|
        sr_rc := SchedOk;
        sr_state := s';
      |} ->
    wake (abstract_of_concrete s) tid gen =
      Some (abstract_of_concrete s').
Proof.
  intros s tid gen s' Hstep.
  unfold sched_wake in Hstep.
  destruct (concrete_valid_thread s tid) eqn:Hbounds; try discriminate.
  destruct (concrete_lookup_thread s tid) as [thread |] eqn:Hlookup; try discriminate.
  destruct (Nat.eqb (ct_generation thread) gen) eqn:Hgen; try discriminate.
  destruct (ct_status thread) eqn:Hstatus; try discriminate.
  inversion Hstep; subst; clear Hstep.
  unfold wake.
  rewrite (concrete_lookup_thread_refines s tid thread Hlookup).
  unfold generation_matches.
  simpl.
  rewrite Hgen.
  rewrite Hstatus.
  rewrite abstract_sync_replace_concrete_thread.
  reflexivity.
Qed.

Lemma sched_exit_thread_refines_abstract :
  forall s tid gen s',
    sched_exit_thread s tid gen =
      {|
        sr_rc := SchedOk;
        sr_state := s';
      |} ->
    exit_thread (abstract_of_concrete s) tid gen =
      Some (abstract_of_concrete s').
Proof.
  intros s tid gen s' Hstep.
  unfold sched_exit_thread in Hstep.
  destruct (concrete_valid_thread s tid) eqn:Hbounds; try discriminate.
  destruct (concrete_lookup_thread s tid) as [thread |] eqn:Hlookup; try discriminate.
  destruct (Nat.eqb (ct_generation thread) gen) eqn:Hgen; try discriminate.
  destruct (c_is_live_status (ct_status thread)) eqn:Hlive; try discriminate.
  inversion Hstep; subst; clear Hstep.
  unfold exit_thread.
  rewrite (concrete_lookup_thread_refines s tid thread Hlookup).
  unfold generation_matches.
  simpl.
  rewrite Hgen.
  destruct (ct_status thread); simpl in Hlive; try discriminate; simpl;
    rewrite abstract_sync_replace_concrete_thread; reflexivity.
Qed.

Lemma sched_commit_failed_unchanged :
  forall s cpu tid gen,
    sr_rc (sched_commit s cpu tid gen) <> SchedOk ->
    sr_state (sched_commit s cpu tid gen) = s.
Proof.
  intros s cpu tid gen Hfailed.
  unfold sched_commit in *.
  destruct (andb (concrete_valid_cpu s cpu) (concrete_valid_thread s tid));
    simpl in *; try reflexivity.
  destruct (negb (concrete_cpu_has_owner s cpu));
    simpl in *; try reflexivity.
  destruct (concrete_lookup_thread s tid) as [thread |];
    simpl in *; try reflexivity.
  destruct (Nat.eqb (ct_generation thread) gen);
    simpl in *; try reflexivity.
  destruct (ct_status thread);
    simpl in *; try reflexivity.
  exfalso.
  apply Hfailed.
  reflexivity.
Qed.

Lemma sched_claim_failed_unchanged :
  forall s cpu tid gen,
    sr_rc (sched_claim s cpu tid gen) <> SchedOk ->
    sr_state (sched_claim s cpu tid gen) = s.
Proof.
  intros s cpu tid gen Hfailed.
  unfold sched_claim in *.
  destruct (andb (concrete_valid_cpu s cpu) (concrete_valid_thread s tid));
    simpl in *; try reflexivity.
  destruct (concrete_lookup_thread s tid) as [thread |];
    simpl in *; try reflexivity.
  destruct (Nat.eqb (ct_generation thread) gen);
    simpl in *; try reflexivity.
  destruct (ct_status thread);
    simpl in *; try reflexivity.
  destruct (Nat.eqb (ct_cpu thread) cpu);
    simpl in *; try reflexivity.
  exfalso.
  apply Hfailed.
  reflexivity.
Qed.

Lemma sched_preempt_failed_unchanged :
  forall s cpu tid gen,
    sr_rc (sched_preempt s cpu tid gen) <> SchedOk ->
    sr_state (sched_preempt s cpu tid gen) = s.
Proof.
  intros s cpu tid gen Hfailed.
  unfold sched_preempt in *.
  destruct (andb (concrete_valid_cpu s cpu) (concrete_valid_thread s tid));
    simpl in *; try reflexivity.
  destruct (concrete_lookup_thread s tid) as [thread |];
    simpl in *; try reflexivity.
  destruct (Nat.eqb (ct_generation thread) gen);
    simpl in *; try reflexivity.
  destruct (ct_status thread);
    simpl in *; try reflexivity.
  destruct (Nat.eqb (ct_cpu thread) cpu);
    simpl in *; try reflexivity.
  exfalso.
  apply Hfailed.
  reflexivity.
Qed.

Lemma sched_block_failed_unchanged :
  forall s cpu tid gen,
    sr_rc (sched_block s cpu tid gen) <> SchedOk ->
    sr_state (sched_block s cpu tid gen) = s.
Proof.
  intros s cpu tid gen Hfailed.
  unfold sched_block in *.
  destruct (andb (concrete_valid_cpu s cpu) (concrete_valid_thread s tid));
    simpl in *; try reflexivity.
  destruct (concrete_lookup_thread s tid) as [thread |];
    simpl in *; try reflexivity.
  destruct (Nat.eqb (ct_generation thread) gen);
    simpl in *; try reflexivity.
  destruct (ct_status thread);
    simpl in *; try reflexivity.
  destruct (Nat.eqb (ct_cpu thread) cpu);
    simpl in *; try reflexivity.
  exfalso.
  apply Hfailed.
  reflexivity.
Qed.

Lemma sched_wake_failed_unchanged :
  forall s tid gen,
    sr_rc (sched_wake s tid gen) <> SchedOk ->
    sr_state (sched_wake s tid gen) = s.
Proof.
  intros s tid gen Hfailed.
  unfold sched_wake in *.
  destruct (concrete_valid_thread s tid); simpl in *; try reflexivity.
  destruct (concrete_lookup_thread s tid) as [thread |];
    simpl in *; try reflexivity.
  destruct (Nat.eqb (ct_generation thread) gen);
    simpl in *; try reflexivity.
  destruct (ct_status thread);
    simpl in *; try reflexivity.
  exfalso.
  apply Hfailed.
  reflexivity.
Qed.

Lemma sched_exit_thread_failed_unchanged :
  forall s tid gen,
    sr_rc (sched_exit_thread s tid gen) <> SchedOk ->
    sr_state (sched_exit_thread s tid gen) = s.
Proof.
  intros s tid gen Hfailed.
  unfold sched_exit_thread in *.
  destruct (concrete_valid_thread s tid); simpl in *; try reflexivity.
  destruct (concrete_lookup_thread s tid) as [thread |];
    simpl in *; try reflexivity.
  destruct (Nat.eqb (ct_generation thread) gen);
    simpl in *; try reflexivity.
  destruct (c_is_live_status (ct_status thread));
    simpl in *; try reflexivity.
  exfalso.
  apply Hfailed.
  reflexivity.
Qed.

Theorem sched_commit_cpu_bounds :
  forall s cpu tid gen,
    concrete_valid_cpu s cpu = false ->
    sched_commit s cpu tid gen = fail SchedErrBounds s.
Proof.
  intros s cpu tid gen Hcpu.
  unfold sched_commit.
  rewrite Hcpu.
  reflexivity.
Qed.

Theorem sched_commit_thread_bounds :
  forall s cpu tid gen,
    concrete_valid_thread s tid = false ->
    sched_commit s cpu tid gen = fail SchedErrBounds s.
Proof.
  intros s cpu tid gen Htid.
  unfold sched_commit.
  rewrite Htid.
  destruct (concrete_valid_cpu s cpu); reflexivity.
Qed.

Theorem sched_claim_cpu_bounds :
  forall s cpu tid gen,
    concrete_valid_cpu s cpu = false ->
    sched_claim s cpu tid gen = fail SchedErrBounds s.
Proof.
  intros s cpu tid gen Hcpu.
  unfold sched_claim.
  rewrite Hcpu.
  reflexivity.
Qed.

Theorem sched_claim_thread_bounds :
  forall s cpu tid gen,
    concrete_valid_thread s tid = false ->
    sched_claim s cpu tid gen = fail SchedErrBounds s.
Proof.
  intros s cpu tid gen Htid.
  unfold sched_claim.
  rewrite Htid.
  destruct (concrete_valid_cpu s cpu); reflexivity.
Qed.

Theorem sched_preempt_cpu_bounds :
  forall s cpu tid gen,
    concrete_valid_cpu s cpu = false ->
    sched_preempt s cpu tid gen = fail SchedErrBounds s.
Proof.
  intros s cpu tid gen Hcpu.
  unfold sched_preempt.
  rewrite Hcpu.
  reflexivity.
Qed.

Theorem sched_preempt_thread_bounds :
  forall s cpu tid gen,
    concrete_valid_thread s tid = false ->
    sched_preempt s cpu tid gen = fail SchedErrBounds s.
Proof.
  intros s cpu tid gen Htid.
  unfold sched_preempt.
  rewrite Htid.
  destruct (concrete_valid_cpu s cpu); reflexivity.
Qed.

Theorem sched_block_cpu_bounds :
  forall s cpu tid gen,
    concrete_valid_cpu s cpu = false ->
    sched_block s cpu tid gen = fail SchedErrBounds s.
Proof.
  intros s cpu tid gen Hcpu.
  unfold sched_block.
  rewrite Hcpu.
  reflexivity.
Qed.

Theorem sched_block_thread_bounds :
  forall s cpu tid gen,
    concrete_valid_thread s tid = false ->
    sched_block s cpu tid gen = fail SchedErrBounds s.
Proof.
  intros s cpu tid gen Htid.
  unfold sched_block.
  rewrite Htid.
  destruct (concrete_valid_cpu s cpu); reflexivity.
Qed.

Theorem sched_wake_thread_bounds :
  forall s tid gen,
    concrete_valid_thread s tid = false ->
    sched_wake s tid gen = fail SchedErrBounds s.
Proof.
  intros s tid gen Htid.
  unfold sched_wake.
  rewrite Htid.
  reflexivity.
Qed.

Theorem sched_exit_thread_bounds :
  forall s tid gen,
    concrete_valid_thread s tid = false ->
    sched_exit_thread s tid gen = fail SchedErrBounds s.
Proof.
  intros s tid gen Htid.
  unfold sched_exit_thread.
  rewrite Htid.
  reflexivity.
Qed.

Theorem sched_commit_generation_mismatch :
  forall s cpu tid gen thread,
    andb (concrete_valid_cpu s cpu) (concrete_valid_thread s tid) = true ->
    concrete_cpu_has_owner s cpu = false ->
    concrete_lookup_thread s tid = Some thread ->
    Nat.eqb (ct_generation thread) gen = false ->
    sched_commit s cpu tid gen = fail SchedErrGeneration s.
Proof.
  intros s cpu tid gen thread Hbounds Hownerless Hlookup Hgen.
  unfold sched_commit.
  rewrite Hbounds.
  rewrite Hownerless.
  rewrite Hlookup.
  rewrite Hgen.
  reflexivity.
Qed.

Theorem sched_claim_generation_mismatch :
  forall s cpu tid gen thread,
    andb (concrete_valid_cpu s cpu) (concrete_valid_thread s tid) = true ->
    concrete_lookup_thread s tid = Some thread ->
    Nat.eqb (ct_generation thread) gen = false ->
    sched_claim s cpu tid gen = fail SchedErrGeneration s.
Proof.
  intros s cpu tid gen thread Hbounds Hlookup Hgen.
  unfold sched_claim.
  rewrite Hbounds.
  rewrite Hlookup.
  rewrite Hgen.
  reflexivity.
Qed.

Theorem sched_preempt_generation_mismatch :
  forall s cpu tid gen thread,
    andb (concrete_valid_cpu s cpu) (concrete_valid_thread s tid) = true ->
    concrete_lookup_thread s tid = Some thread ->
    Nat.eqb (ct_generation thread) gen = false ->
    sched_preempt s cpu tid gen = fail SchedErrGeneration s.
Proof.
  intros s cpu tid gen thread Hbounds Hlookup Hgen.
  unfold sched_preempt.
  rewrite Hbounds.
  rewrite Hlookup.
  rewrite Hgen.
  reflexivity.
Qed.

Theorem sched_block_generation_mismatch :
  forall s cpu tid gen thread,
    andb (concrete_valid_cpu s cpu) (concrete_valid_thread s tid) = true ->
    concrete_lookup_thread s tid = Some thread ->
    Nat.eqb (ct_generation thread) gen = false ->
    sched_block s cpu tid gen = fail SchedErrGeneration s.
Proof.
  intros s cpu tid gen thread Hbounds Hlookup Hgen.
  unfold sched_block.
  rewrite Hbounds.
  rewrite Hlookup.
  rewrite Hgen.
  reflexivity.
Qed.

Theorem sched_wake_generation_mismatch :
  forall s tid gen thread,
    concrete_valid_thread s tid = true ->
    concrete_lookup_thread s tid = Some thread ->
    Nat.eqb (ct_generation thread) gen = false ->
    sched_wake s tid gen = fail SchedErrGeneration s.
Proof.
  intros s tid gen thread Hbounds Hlookup Hgen.
  unfold sched_wake.
  rewrite Hbounds.
  rewrite Hlookup.
  rewrite Hgen.
  reflexivity.
Qed.

Theorem sched_exit_thread_generation_mismatch :
  forall s tid gen thread,
    concrete_valid_thread s tid = true ->
    concrete_lookup_thread s tid = Some thread ->
    Nat.eqb (ct_generation thread) gen = false ->
    sched_exit_thread s tid gen = fail SchedErrGeneration s.
Proof.
  intros s tid gen thread Hbounds Hlookup Hgen.
  unfold sched_exit_thread.
  rewrite Hbounds.
  rewrite Hlookup.
  rewrite Hgen.
  reflexivity.
Qed.

Theorem concrete_running_cpu_valid :
  forall s tid thread,
    concrete_invariant s ->
    concrete_lookup_thread_raw s tid = Some thread ->
    ct_status thread = CRunning ->
    ct_cpu thread < cs_cpu_count s.
Proof.
  intros s tid thread [_ [Hprotocol [_ _]]] Hlookup Hstatus.
  destruct thread as [gen status owner].
  simpl in *.
  subst status.
  destruct Hprotocol as [Hvalid _].
  unfold status_cpu_valid in Hvalid.
  pose proof (concrete_lookup_thread_raw_refines s tid
    {| ct_generation := gen; ct_status := CRunning; ct_cpu := owner |}
    Hlookup) as Habs_lookup.
  eapply Hvalid; eauto.
Qed.

Theorem concrete_pending_cpu_valid :
  forall s tid thread,
    concrete_invariant s ->
    concrete_lookup_thread_raw s tid = Some thread ->
    ct_status thread = CPending ->
    ct_cpu thread < cs_cpu_count s.
Proof.
  intros s tid thread [_ [Hprotocol [_ _]]] Hlookup Hstatus.
  destruct thread as [gen status owner].
  simpl in *.
  subst status.
  destruct Hprotocol as [Hvalid _].
  unfold status_cpu_valid in Hvalid.
  pose proof (concrete_lookup_thread_raw_refines s tid
    {| ct_generation := gen; ct_status := CPending; ct_cpu := owner |}
    Hlookup) as Habs_lookup.
  eapply Hvalid; eauto.
Qed.

Theorem concrete_inactive_thread_empty :
  forall s tid thread,
    concrete_invariant s ->
    concrete_lookup_thread_raw s tid = Some thread ->
    cs_thread_count s <= tid ->
    ct_status thread = CEmpty.
Proof.
  intros s tid thread [_ [_ [Hinactive _]]] Hlookup Htail.
  eapply Hinactive; eauto.
Qed.

Theorem concrete_running_thread_active :
  forall s tid thread,
    concrete_invariant s ->
    concrete_lookup_thread_raw s tid = Some thread ->
    ct_status thread = CRunning ->
    tid < cs_thread_count s.
Proof.
  intros s tid thread Hinv Hlookup Hstatus.
  destruct (Nat.leb (cs_thread_count s) tid) eqn:Htailb.
  - apply Nat.leb_le in Htailb.
    pose proof (concrete_inactive_thread_empty s tid thread Hinv Hlookup Htailb)
      as Hempty.
    rewrite Hstatus in Hempty.
    discriminate.
  - apply Nat.leb_gt.
    exact Htailb.
Qed.

Theorem concrete_pending_thread_active :
  forall s tid thread,
    concrete_invariant s ->
    concrete_lookup_thread_raw s tid = Some thread ->
    ct_status thread = CPending ->
    tid < cs_thread_count s.
Proof.
  intros s tid thread Hinv Hlookup Hstatus.
  destruct (Nat.leb (cs_thread_count s) tid) eqn:Htailb.
  - apply Nat.leb_le in Htailb.
    pose proof (concrete_inactive_thread_empty s tid thread Hinv Hlookup Htailb)
      as Hempty.
    rewrite Hstatus in Hempty.
    discriminate.
  - apply Nat.leb_gt.
    exact Htailb.
Qed.

Theorem concrete_no_cpu_double_running :
  forall s tid1 tid2 thread1 thread2 cpu,
    concrete_invariant s ->
    concrete_lookup_thread_raw s tid1 = Some thread1 ->
    concrete_lookup_thread_raw s tid2 = Some thread2 ->
    ct_status thread1 = CRunning ->
    ct_status thread2 = CRunning ->
    ct_cpu thread1 = cpu ->
    ct_cpu thread2 = cpu ->
    tid1 = tid2.
Proof.
  intros s tid1 tid2 thread1 thread2 cpu [_ [Hprotocol [_ _]]]
    Hlookup1 Hlookup2 Hstatus1 Hstatus2 Hcpu1 Hcpu2.
  destruct thread1 as [gen1 status1 owner1].
  destruct thread2 as [gen2 status2 owner2].
  simpl in *.
  subst status1 status2 owner1 owner2.
  destruct Hprotocol as [_ [Hrunning _]].
  unfold no_cpu_double_running in Hrunning.
  pose proof (concrete_lookup_thread_raw_refines s tid1
    {| ct_generation := gen1; ct_status := CRunning; ct_cpu := cpu |}
    Hlookup1) as Habs_lookup1.
  pose proof (concrete_lookup_thread_raw_refines s tid2
    {| ct_generation := gen2; ct_status := CRunning; ct_cpu := cpu |}
    Hlookup2) as Habs_lookup2.
  eapply Hrunning; eauto; reflexivity.
Qed.

Theorem concrete_no_cpu_double_pending :
  forall s tid1 tid2 thread1 thread2 cpu,
    concrete_invariant s ->
    concrete_lookup_thread_raw s tid1 = Some thread1 ->
    concrete_lookup_thread_raw s tid2 = Some thread2 ->
    ct_status thread1 = CPending ->
    ct_status thread2 = CPending ->
    ct_cpu thread1 = cpu ->
    ct_cpu thread2 = cpu ->
    tid1 = tid2.
Proof.
  intros s tid1 tid2 thread1 thread2 cpu [_ [Hprotocol [_ _]]]
    Hlookup1 Hlookup2 Hstatus1 Hstatus2 Hcpu1 Hcpu2.
  destruct thread1 as [gen1 status1 owner1].
  destruct thread2 as [gen2 status2 owner2].
  simpl in *.
  subst status1 status2 owner1 owner2.
  destruct Hprotocol as [_ [_ [Hpending _]]].
  unfold no_cpu_double_pending in Hpending.
  pose proof (concrete_lookup_thread_raw_refines s tid1
    {| ct_generation := gen1; ct_status := CPending; ct_cpu := cpu |}
    Hlookup1) as Habs_lookup1.
  pose proof (concrete_lookup_thread_raw_refines s tid2
    {| ct_generation := gen2; ct_status := CPending; ct_cpu := cpu |}
    Hlookup2) as Habs_lookup2.
  eapply Hpending; eauto; reflexivity.
Qed.

Theorem concrete_no_cpu_running_pending_overlap :
  forall s running_tid pending_tid running_thread pending_thread cpu,
    concrete_invariant s ->
    concrete_lookup_thread_raw s running_tid = Some running_thread ->
    concrete_lookup_thread_raw s pending_tid = Some pending_thread ->
    ct_status running_thread = CRunning ->
    ct_status pending_thread = CPending ->
    ct_cpu running_thread = cpu ->
    ct_cpu pending_thread = cpu ->
    False.
Proof.
  intros s running_tid pending_tid running_thread pending_thread cpu
    [_ [Hprotocol [_ _]]] Hrunning_lookup Hpending_lookup
    Hrunning_status Hpending_status Hrunning_cpu Hpending_cpu.
  destruct running_thread as [running_gen running_status' running_owner].
  destruct pending_thread as [pending_gen pending_status' pending_owner].
  simpl in *.
  subst running_status' pending_status' running_owner pending_owner.
  destruct Hprotocol as [_ [_ [_ Hoverlap]]].
  unfold no_cpu_running_pending_overlap in Hoverlap.
  pose proof (concrete_lookup_thread_raw_refines s running_tid
    {| ct_generation := running_gen; ct_status := CRunning; ct_cpu := cpu |}
    Hrunning_lookup) as Habs_running_lookup.
  pose proof (concrete_lookup_thread_raw_refines s pending_tid
    {| ct_generation := pending_gen; ct_status := CPending; ct_cpu := cpu |}
    Hpending_lookup) as Habs_pending_lookup.
  exact (Hoverlap
    running_tid
    pending_tid
    (concrete_thread_to_abstract
      {| ct_generation := running_gen; ct_status := CRunning; ct_cpu := cpu |})
    (concrete_thread_to_abstract
      {| ct_generation := pending_gen; ct_status := CPending; ct_cpu := cpu |})
    cpu
    Habs_running_lookup
    Habs_pending_lookup
    eq_refl
    eq_refl).
Qed.

Lemma running_thread_for_from_some :
  forall remaining threads cpu base tid gen,
    running_thread_for_from remaining threads cpu base = Some (tid, gen) ->
    exists index thread,
      tid = base + index /\
      index < remaining /\
      nth_error threads index = Some thread /\
      ct_status thread = CRunning /\
      ct_cpu thread = cpu /\
      ct_generation thread = gen.
Proof.
  induction remaining as [| remaining IH];
    intros threads cpu base tid gen Hrun;
    destruct threads as [| thread rest]; simpl in Hrun; try discriminate.
  destruct
    (andb
      match ct_status thread with
      | CRunning => true
      | _ => false
      end
      (Nat.eqb (ct_cpu thread) cpu)) eqn:Hmatch.
  - apply andb_true_iff in Hmatch as [Hstatus Hcpu].
    destruct thread as [thread_gen thread_status thread_cpu].
    simpl in *.
    destruct thread_status; try discriminate.
    apply Nat.eqb_eq in Hcpu.
    inversion Hrun; subst.
    exists 0.
    exists {| ct_generation := gen; ct_status := CRunning; ct_cpu := cpu |}.
    repeat split; simpl; auto; lia.
  - destruct (IH rest cpu (S base) tid gen Hrun)
      as [index [found [Htid [Hindex [Hlookup [Hstatus [Hcpu Hgen]]]]]]].
    exists (S index).
    exists found.
    repeat split; simpl; auto; lia.
Qed.

Lemma running_thread_for_from_finds_some :
  forall remaining threads cpu base index thread,
    index < remaining ->
    nth_error threads index = Some thread ->
    ct_status thread = CRunning ->
    ct_cpu thread = cpu ->
    exists tid gen,
      running_thread_for_from remaining threads cpu base = Some (tid, gen).
Proof.
  induction remaining as [| remaining IH];
    intros threads cpu base index thread Hindex Hlookup Hstatus Hcpu.
  - lia.
  - destruct threads as [| head rest].
    + destruct index; discriminate.
    + simpl.
      simpl in Hlookup.
  destruct index as [| index'].
      * inversion Hlookup; subst head.
        rewrite Hstatus.
        rewrite Hcpu.
        rewrite Nat.eqb_refl.
        exists base.
        exists (ct_generation thread).
        reflexivity.
      * destruct
          (andb
            match ct_status head with
            | CRunning => true
            | _ => false
            end
            (Nat.eqb (ct_cpu head) cpu)) eqn:Hhead.
        -- exists base.
           exists (ct_generation head).
           reflexivity.
        -- specialize (IH rest cpu (S base) index' thread).
           apply IH; auto; lia.
Qed.

Lemma running_thread_for_some :
  forall s cpu tid gen,
    running_thread_for s cpu = Some (tid, gen) ->
    exists thread,
      concrete_lookup_thread_raw s tid = Some thread /\
      ct_status thread = CRunning /\
      ct_cpu thread = cpu /\
      ct_generation thread = gen.
Proof.
  intros s cpu tid gen Hrun.
  unfold running_thread_for in Hrun.
  destruct (concrete_valid_cpu s cpu) eqn:Hcpu_valid; try discriminate.
  destruct (running_thread_for_from_some
    (cs_thread_count s)
    (cs_threads s)
    cpu
    0
    tid
    gen
    Hrun) as [index [thread [Htid [_ [Hlookup [Hstatus [Hcpu Hgen]]]]]]].
  simpl in Htid.
  subst tid.
  exists thread.
  repeat split; auto.
Qed.

Lemma running_thread_for_active_running_some :
  forall s tid thread cpu,
    concrete_valid_cpu s cpu = true ->
    concrete_lookup_thread s tid = Some thread ->
    ct_status thread = CRunning ->
    ct_cpu thread = cpu ->
    exists found_tid found_gen,
      running_thread_for s cpu = Some (found_tid, found_gen).
Proof.
  intros s tid thread cpu Hcpu_valid Hlookup Hstatus Hcpu.
  apply concrete_lookup_thread_parts in Hlookup as [Hvalid Hraw].
  unfold running_thread_for.
  rewrite Hcpu_valid.
  unfold concrete_lookup_thread_raw in Hraw.
  unfold concrete_valid_thread in Hvalid.
  apply Nat.ltb_lt in Hvalid.
  eapply running_thread_for_from_finds_some; eauto.
Qed.

Theorem concrete_cpu_current_points_to_running :
  forall s cpu entry,
    concrete_invariant s ->
    concrete_lookup_cpu_raw s cpu = Some entry ->
    cc_current_tid entry <> invalid_thread_id ->
    exists thread,
      concrete_lookup_thread_raw s (cc_current_tid entry) = Some thread /\
      ct_status thread = CRunning /\
      ct_cpu thread = cpu /\
      ct_generation thread = cc_current_generation entry.
Proof.
  intros s cpu entry [[_ [_ [_ Hcpus_len]]] [_ [_ Htable]]] Hlookup Hcurrent.
  unfold concrete_lookup_cpu_raw in Hlookup.
  rewrite Htable in Hlookup.
  unfold computed_cpus in Hlookup.
  rewrite nth_error_map in Hlookup.
  rewrite nth_error_seq in Hlookup.
  destruct (cpu <? max_cpus) eqn:Hcpu_max; try discriminate.
  simpl in Hlookup.
  inversion Hlookup; subst entry.
  unfold concrete_cpu_for in Hcurrent |- *.
  destruct (running_thread_for s cpu) as [[tid gen] |] eqn:Hrun.
  - simpl in *.
    apply running_thread_for_some in Hrun.
    exact Hrun.
  - simpl in Hcurrent.
    exfalso.
    apply Hcurrent.
    reflexivity.
Qed.

Theorem concrete_running_points_to_cpu_current :
  forall s tid thread cpu,
    concrete_invariant s ->
    concrete_lookup_thread s tid = Some thread ->
    ct_status thread = CRunning ->
    ct_cpu thread = cpu ->
    exists entry,
      concrete_lookup_cpu_raw s cpu = Some entry /\
      cc_current_tid entry = tid /\
      cc_current_generation entry = ct_generation thread.
Proof.
  intros s tid thread cpu Hinv Hlookup Hstatus Hcpu.
  destruct Hinv as [Hshape [Hprotocol [Hinactive Htable]]].
  destruct Hshape as [Hcpu_count [Hthread_count [Hthreads_len Hcpus_len]]].
  assert (Hcpu_lt: cpu < cs_cpu_count s).
  {
    pose proof (concrete_lookup_thread_parts s tid thread Hlookup) as [_ Hraw].
    pose proof (concrete_running_cpu_valid s tid thread
      (conj (conj Hcpu_count (conj Hthread_count (conj Hthreads_len Hcpus_len)))
      (conj Hprotocol (conj Hinactive Htable)))
      Hraw
      Hstatus) as Hthread_cpu_lt.
    rewrite Hcpu in Hthread_cpu_lt.
    exact Hthread_cpu_lt.
  }
  assert (Hcpu_valid: concrete_valid_cpu s cpu = true).
  {
    unfold concrete_valid_cpu.
    apply Nat.ltb_lt.
    exact Hcpu_lt.
  }
  destruct (running_thread_for_active_running_some
    s tid thread cpu Hcpu_valid Hlookup Hstatus Hcpu)
    as [found_tid [found_gen Hrun]].
  pose proof Hrun as Hrun_some.
  apply running_thread_for_some in Hrun_some as
    [found_thread [Hfound_lookup [Hfound_status [Hfound_cpu Hfound_gen]]]].
  assert (Hsame_tid: found_tid = tid).
  {
    symmetry.
    pose proof (concrete_lookup_thread_parts s tid thread Hlookup) as [_ Hraw].
    eapply concrete_no_cpu_double_running with
      (thread1 := thread)
      (thread2 := found_thread)
      (cpu := cpu); eauto.
    exact (conj (conj Hcpu_count (conj Hthread_count (conj Hthreads_len Hcpus_len)))
      (conj Hprotocol (conj Hinactive Htable))).
  }
  subst found_tid.
  pose proof (concrete_lookup_thread_parts s tid thread Hlookup) as [_ Hraw].
  rewrite Hraw in Hfound_lookup.
  inversion Hfound_lookup; subst found_thread.
  rewrite <- Hfound_gen in Hrun.
  exists (concrete_cpu_for s cpu).
  split.
  - unfold concrete_lookup_cpu_raw.
    rewrite Htable.
    unfold computed_cpus.
    rewrite nth_error_map.
    rewrite nth_error_seq.
    assert (Hcpu_max: cpu < max_cpus) by lia.
    rewrite (proj2 (Nat.ltb_lt cpu max_cpus) Hcpu_max).
    simpl.
    reflexivity.
  - unfold concrete_cpu_for.
    rewrite Hrun.
    simpl.
    auto.
Qed.

Theorem concrete_raw_running_points_to_cpu_current :
  forall s tid thread cpu,
    concrete_invariant s ->
    concrete_lookup_thread_raw s tid = Some thread ->
    ct_status thread = CRunning ->
    ct_cpu thread = cpu ->
    exists entry,
      concrete_lookup_cpu_raw s cpu = Some entry /\
      cc_current_tid entry = tid /\
      cc_current_generation entry = ct_generation thread.
Proof.
  intros s tid thread cpu Hinv Hlookup Hstatus Hcpu.
  assert (Hactive: tid < cs_thread_count s).
  {
    eapply concrete_running_thread_active; eauto.
  }
  assert (Hvalid: concrete_valid_thread s tid = true).
  {
    unfold concrete_valid_thread.
    apply Nat.ltb_lt.
    exact Hactive.
  }
  eapply concrete_running_points_to_cpu_current; eauto.
  unfold concrete_lookup_thread.
  rewrite Hvalid.
  exact Hlookup.
Qed.

Theorem concrete_cpu_current_invalid_has_no_active_running :
  forall s cpu entry tid thread,
    concrete_invariant s ->
    concrete_lookup_cpu_raw s cpu = Some entry ->
    cc_current_tid entry = invalid_thread_id ->
    concrete_lookup_thread s tid = Some thread ->
    ct_status thread = CRunning ->
    ct_cpu thread = cpu ->
    False.
Proof.
  intros s cpu entry tid thread Hinv Hcpu_lookup Hcurrent_invalid
    Hthread_lookup Hstatus Hcpu.
  destruct (concrete_running_points_to_cpu_current
    s tid thread cpu Hinv Hthread_lookup Hstatus Hcpu)
    as [computed_entry [Hcomputed_lookup [Hcomputed_tid _]]].
  rewrite Hcpu_lookup in Hcomputed_lookup.
  inversion Hcomputed_lookup; subst computed_entry.
  rewrite Hcurrent_invalid in Hcomputed_tid.
  assert (Htid_lt: tid < cs_thread_count s).
  {
    eapply concrete_lookup_thread_tid_lt_count.
    exact Hthread_lookup.
  }
  destruct Hinv as [[_ [Hthread_count _]] _].
  symmetry in Hcomputed_tid.
  subst tid.
  unfold invalid_thread_id, max_threads in Htid_lt.
  unfold max_threads in Hthread_count.
  lia.
Qed.

Theorem sched_commit_preserves_concrete_invariant :
  forall s cpu tid gen,
    concrete_invariant s ->
    sr_rc (sched_commit s cpu tid gen) = SchedOk ->
    concrete_invariant (sr_state (sched_commit s cpu tid gen)).
Proof.
  intros s cpu tid gen [Hshape [Hprotocol [Hinactive Htable]]] Hok.
  destruct (sched_commit s cpu tid gen) as [rc s'] eqn:Hstep.
  simpl in Hok.
  subst rc.
  split.
  - unfold sched_commit in Hstep.
    destruct (andb (concrete_valid_cpu s cpu) (concrete_valid_thread s tid)) eqn:?; try discriminate.
    destruct (negb (concrete_cpu_has_owner s cpu)) eqn:?; try discriminate.
    destruct (concrete_lookup_thread s tid) as [thread |] eqn:?; try discriminate.
    destruct (Nat.eqb (ct_generation thread) gen) eqn:?; try discriminate.
    destruct (ct_status thread) eqn:?; try discriminate.
    inversion Hstep; subst.
    apply concrete_shape_sync_replace_thread.
    exact Hshape.
  - split.
    + eapply commit_preserves_protocol_invariant; eauto.
      eapply sched_commit_refines_abstract.
      exact Hstep.
    + split.
      * unfold sched_commit in Hstep.
        destruct (andb (concrete_valid_cpu s cpu) (concrete_valid_thread s tid)) eqn:?; try discriminate.
        destruct (negb (concrete_cpu_has_owner s cpu)) eqn:?; try discriminate.
        destruct (concrete_lookup_thread s tid) as [thread |] eqn:Hlookup; try discriminate.
        destruct (Nat.eqb (ct_generation thread) gen) eqn:?; try discriminate.
        destruct (ct_status thread) eqn:?; try discriminate.
        inversion Hstep; subst.
        eapply inactive_threads_empty_sync_replace_active; eauto.
        eapply concrete_lookup_thread_tid_lt_count.
        exact Hlookup.
      * unfold sched_commit in Hstep.
        destruct (andb (concrete_valid_cpu s cpu) (concrete_valid_thread s tid)) eqn:?; try discriminate.
        destruct (negb (concrete_cpu_has_owner s cpu)) eqn:?; try discriminate.
        destruct (concrete_lookup_thread s tid) as [thread |] eqn:?; try discriminate.
        destruct (Nat.eqb (ct_generation thread) gen) eqn:?; try discriminate.
        destruct (ct_status thread) eqn:?; try discriminate.
        inversion Hstep; subst.
        apply cpu_table_matches_sync.
Qed.

Theorem sched_claim_preserves_concrete_invariant :
  forall s cpu tid gen,
    concrete_invariant s ->
    sr_rc (sched_claim s cpu tid gen) = SchedOk ->
    concrete_invariant (sr_state (sched_claim s cpu tid gen)).
Proof.
  intros s cpu tid gen [Hshape [Hprotocol [Hinactive Htable]]] Hok.
  destruct (sched_claim s cpu tid gen) as [rc s'] eqn:Hstep.
  simpl in Hok.
  subst rc.
  split.
  - unfold sched_claim in Hstep.
    destruct (andb (concrete_valid_cpu s cpu) (concrete_valid_thread s tid)) eqn:?; try discriminate.
    destruct (concrete_lookup_thread s tid) as [thread |] eqn:?; try discriminate.
    destruct (Nat.eqb (ct_generation thread) gen) eqn:?; try discriminate.
    destruct (ct_status thread) eqn:?; try discriminate.
    destruct (Nat.eqb (ct_cpu thread) cpu) eqn:?; try discriminate.
    inversion Hstep; subst.
    apply concrete_shape_sync_replace_thread.
    exact Hshape.
  - split.
    + eapply claim_preserves_protocol_invariant; eauto.
      eapply sched_claim_refines_abstract.
      exact Hstep.
    + split.
      * unfold sched_claim in Hstep.
        destruct (andb (concrete_valid_cpu s cpu) (concrete_valid_thread s tid)) eqn:?; try discriminate.
        destruct (concrete_lookup_thread s tid) as [thread |] eqn:Hlookup; try discriminate.
        destruct (Nat.eqb (ct_generation thread) gen) eqn:?; try discriminate.
        destruct (ct_status thread) eqn:?; try discriminate.
        destruct (Nat.eqb (ct_cpu thread) cpu) eqn:?; try discriminate.
        inversion Hstep; subst.
        eapply inactive_threads_empty_sync_replace_active; eauto.
        eapply concrete_lookup_thread_tid_lt_count.
        exact Hlookup.
      * unfold sched_claim in Hstep.
        destruct (andb (concrete_valid_cpu s cpu) (concrete_valid_thread s tid)) eqn:?; try discriminate.
        destruct (concrete_lookup_thread s tid) as [thread |] eqn:?; try discriminate.
        destruct (Nat.eqb (ct_generation thread) gen) eqn:?; try discriminate.
        destruct (ct_status thread) eqn:?; try discriminate.
        destruct (Nat.eqb (ct_cpu thread) cpu) eqn:?; try discriminate.
        inversion Hstep; subst.
        apply cpu_table_matches_sync.
Qed.

Theorem sched_preempt_preserves_concrete_invariant :
  forall s cpu tid gen,
    concrete_invariant s ->
    sr_rc (sched_preempt s cpu tid gen) = SchedOk ->
    concrete_invariant (sr_state (sched_preempt s cpu tid gen)).
Proof.
  intros s cpu tid gen [Hshape [Hprotocol [Hinactive Htable]]] Hok.
  destruct (sched_preempt s cpu tid gen) as [rc s'] eqn:Hstep.
  simpl in Hok.
  subst rc.
  split.
  - unfold sched_preempt in Hstep.
    destruct (andb (concrete_valid_cpu s cpu) (concrete_valid_thread s tid)) eqn:?; try discriminate.
    destruct (concrete_lookup_thread s tid) as [thread |] eqn:?; try discriminate.
    destruct (Nat.eqb (ct_generation thread) gen) eqn:?; try discriminate.
    destruct (ct_status thread) eqn:?; try discriminate.
    destruct (Nat.eqb (ct_cpu thread) cpu) eqn:?; try discriminate.
    inversion Hstep; subst.
    apply concrete_shape_sync_replace_thread.
    exact Hshape.
  - split.
    + eapply preempt_preserves_protocol_invariant; eauto.
      eapply sched_preempt_refines_abstract.
      exact Hstep.
    + split.
      * unfold sched_preempt in Hstep.
        destruct (andb (concrete_valid_cpu s cpu) (concrete_valid_thread s tid)) eqn:?; try discriminate.
        destruct (concrete_lookup_thread s tid) as [thread |] eqn:Hlookup; try discriminate.
        destruct (Nat.eqb (ct_generation thread) gen) eqn:?; try discriminate.
        destruct (ct_status thread) eqn:?; try discriminate.
        destruct (Nat.eqb (ct_cpu thread) cpu) eqn:?; try discriminate.
        inversion Hstep; subst.
        eapply inactive_threads_empty_sync_replace_active; eauto.
        eapply concrete_lookup_thread_tid_lt_count.
        exact Hlookup.
      * unfold sched_preempt in Hstep.
        destruct (andb (concrete_valid_cpu s cpu) (concrete_valid_thread s tid)) eqn:?; try discriminate.
        destruct (concrete_lookup_thread s tid) as [thread |] eqn:?; try discriminate.
        destruct (Nat.eqb (ct_generation thread) gen) eqn:?; try discriminate.
        destruct (ct_status thread) eqn:?; try discriminate.
        destruct (Nat.eqb (ct_cpu thread) cpu) eqn:?; try discriminate.
        inversion Hstep; subst.
        apply cpu_table_matches_sync.
Qed.

Theorem sched_block_preserves_concrete_invariant :
  forall s cpu tid gen,
    concrete_invariant s ->
    sr_rc (sched_block s cpu tid gen) = SchedOk ->
    concrete_invariant (sr_state (sched_block s cpu tid gen)).
Proof.
  intros s cpu tid gen [Hshape [Hprotocol [Hinactive Htable]]] Hok.
  destruct (sched_block s cpu tid gen) as [rc s'] eqn:Hstep.
  simpl in Hok.
  subst rc.
  split.
  - unfold sched_block in Hstep.
    destruct (andb (concrete_valid_cpu s cpu) (concrete_valid_thread s tid)) eqn:?; try discriminate.
    destruct (concrete_lookup_thread s tid) as [thread |] eqn:?; try discriminate.
    destruct (Nat.eqb (ct_generation thread) gen) eqn:?; try discriminate.
    destruct (ct_status thread) eqn:?; try discriminate.
    destruct (Nat.eqb (ct_cpu thread) cpu) eqn:?; try discriminate.
    inversion Hstep; subst.
    apply concrete_shape_sync_replace_thread.
    exact Hshape.
  - split.
    + eapply block_preserves_protocol_invariant; eauto.
      eapply sched_block_refines_abstract.
      exact Hstep.
    + split.
      * unfold sched_block in Hstep.
        destruct (andb (concrete_valid_cpu s cpu) (concrete_valid_thread s tid)) eqn:?; try discriminate.
        destruct (concrete_lookup_thread s tid) as [thread |] eqn:Hlookup; try discriminate.
        destruct (Nat.eqb (ct_generation thread) gen) eqn:?; try discriminate.
        destruct (ct_status thread) eqn:?; try discriminate.
        destruct (Nat.eqb (ct_cpu thread) cpu) eqn:?; try discriminate.
        inversion Hstep; subst.
        eapply inactive_threads_empty_sync_replace_active; eauto.
        eapply concrete_lookup_thread_tid_lt_count.
        exact Hlookup.
      * unfold sched_block in Hstep.
        destruct (andb (concrete_valid_cpu s cpu) (concrete_valid_thread s tid)) eqn:?; try discriminate.
        destruct (concrete_lookup_thread s tid) as [thread |] eqn:?; try discriminate.
        destruct (Nat.eqb (ct_generation thread) gen) eqn:?; try discriminate.
        destruct (ct_status thread) eqn:?; try discriminate.
        destruct (Nat.eqb (ct_cpu thread) cpu) eqn:?; try discriminate.
        inversion Hstep; subst.
        apply cpu_table_matches_sync.
Qed.

Theorem sched_wake_preserves_concrete_invariant :
  forall s tid gen,
    concrete_invariant s ->
    sr_rc (sched_wake s tid gen) = SchedOk ->
    concrete_invariant (sr_state (sched_wake s tid gen)).
Proof.
  intros s tid gen [Hshape [Hprotocol [Hinactive Htable]]] Hok.
  destruct (sched_wake s tid gen) as [rc s'] eqn:Hstep.
  simpl in Hok.
  subst rc.
  split.
  - unfold sched_wake in Hstep.
    destruct (concrete_valid_thread s tid) eqn:?; try discriminate.
    destruct (concrete_lookup_thread s tid) as [thread |] eqn:?; try discriminate.
    destruct (Nat.eqb (ct_generation thread) gen) eqn:?; try discriminate.
    destruct (ct_status thread) eqn:?; try discriminate.
    inversion Hstep; subst.
    apply concrete_shape_sync_replace_thread.
    exact Hshape.
  - split.
    + eapply wake_preserves_protocol_invariant; eauto.
      eapply sched_wake_refines_abstract.
      exact Hstep.
    + split.
      * unfold sched_wake in Hstep.
        destruct (concrete_valid_thread s tid) eqn:?; try discriminate.
        destruct (concrete_lookup_thread s tid) as [thread |] eqn:Hlookup; try discriminate.
        destruct (Nat.eqb (ct_generation thread) gen) eqn:?; try discriminate.
        destruct (ct_status thread) eqn:?; try discriminate.
        inversion Hstep; subst.
        eapply inactive_threads_empty_sync_replace_active; eauto.
        eapply concrete_lookup_thread_tid_lt_count.
        exact Hlookup.
      * unfold sched_wake in Hstep.
        destruct (concrete_valid_thread s tid) eqn:?; try discriminate.
        destruct (concrete_lookup_thread s tid) as [thread |] eqn:?; try discriminate.
        destruct (Nat.eqb (ct_generation thread) gen) eqn:?; try discriminate.
        destruct (ct_status thread) eqn:?; try discriminate.
        inversion Hstep; subst.
        apply cpu_table_matches_sync.
Qed.

Theorem sched_exit_thread_preserves_concrete_invariant :
  forall s tid gen,
    concrete_invariant s ->
    sr_rc (sched_exit_thread s tid gen) = SchedOk ->
    concrete_invariant (sr_state (sched_exit_thread s tid gen)).
Proof.
  intros s tid gen [Hshape [Hprotocol [Hinactive Htable]]] Hok.
  destruct (sched_exit_thread s tid gen) as [rc s'] eqn:Hstep.
  simpl in Hok.
  subst rc.
  split.
  - unfold sched_exit_thread in Hstep.
    destruct (concrete_valid_thread s tid) eqn:?; try discriminate.
    destruct (concrete_lookup_thread s tid) as [thread |] eqn:?; try discriminate.
    destruct (Nat.eqb (ct_generation thread) gen) eqn:?; try discriminate.
    destruct (c_is_live_status (ct_status thread)) eqn:?; try discriminate.
    inversion Hstep; subst.
    apply concrete_shape_sync_replace_thread.
    exact Hshape.
  - split.
    + eapply exit_thread_preserves_protocol_invariant; eauto.
      eapply sched_exit_thread_refines_abstract.
      exact Hstep.
    + split.
      * unfold sched_exit_thread in Hstep.
        destruct (concrete_valid_thread s tid) eqn:?; try discriminate.
        destruct (concrete_lookup_thread s tid) as [thread |] eqn:Hlookup; try discriminate.
        destruct (Nat.eqb (ct_generation thread) gen) eqn:?; try discriminate.
        destruct (c_is_live_status (ct_status thread)) eqn:?; try discriminate.
        inversion Hstep; subst.
        eapply inactive_threads_empty_sync_replace_active; eauto.
        eapply concrete_lookup_thread_tid_lt_count.
        exact Hlookup.
      * unfold sched_exit_thread in Hstep.
        destruct (concrete_valid_thread s tid) eqn:?; try discriminate.
        destruct (concrete_lookup_thread s tid) as [thread |] eqn:?; try discriminate.
        destruct (Nat.eqb (ct_generation thread) gen) eqn:?; try discriminate.
        destruct (c_is_live_status (ct_status thread)) eqn:?; try discriminate.
        inversion Hstep; subst.
        apply cpu_table_matches_sync.
Qed.
