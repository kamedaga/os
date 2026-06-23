From Stdlib Require Import Arith.PeanoNat Lia Lists.List.
From Pacha.Filed Require Import
  MulticoreModel
  VfsModel.

Import ListNotations.

Theorem empty_filed_runtime_sequence_log_wf :
  sequence_log_wf
    (frt_next_sequence empty_filed_runtime_state)
    (frt_log empty_filed_runtime_state).
Proof.
  unfold empty_filed_runtime_state, sequence_log_wf.
  simpl.
  split; constructor.
Qed.

Theorem run_filed_request_advances_sequence :
  forall runtime request next_runtime response,
    run_filed_request runtime request = (next_runtime, response) ->
    frt_next_sequence next_runtime = S (frt_next_sequence runtime).
Proof.
  intros runtime request next_runtime response Hrun.
  unfold run_filed_request in Hrun.
  destruct (apply_filed_op (frt_vfs runtime) (frq_op request)) as [next_vfs op_response].
  inversion Hrun.
  reflexivity.
Qed.

Theorem run_filed_request_logs_source :
  forall runtime request next_runtime response entry,
    run_filed_request runtime request = (next_runtime, response) ->
    frt_log next_runtime = entry :: frt_log runtime ->
    fle_core entry = frq_core request /\
    fle_client entry = frq_client request /\
    fle_sequence entry = frt_next_sequence runtime.
Proof.
  intros runtime request next_runtime response entry Hrun Hlog.
  unfold run_filed_request in Hrun.
  destruct (apply_filed_op (frt_vfs runtime) (frq_op request)) as [next_vfs op_response].
  inversion Hrun; subst; clear Hrun.
  inversion Hlog; subst; clear Hlog.
  repeat split; reflexivity.
Qed.

Lemma sequence_not_in_log_if_all_less :
  forall sequence log,
    Forall (fun entry => fle_sequence entry < sequence)%nat log ->
    ~ In sequence (map fle_sequence log).
Proof.
  intros sequence log Hall.
  induction Hall as [| entry rest Hlt Hall IH]; simpl.
  - intros [].
  - intros [Heq | Hin].
    + lia.
    + apply IH.
      exact Hin.
Qed.

Theorem run_filed_request_preserves_sequence_log_wf :
  forall runtime request next_runtime response,
    sequence_log_wf (frt_next_sequence runtime) (frt_log runtime) ->
    run_filed_request runtime request = (next_runtime, response) ->
    sequence_log_wf (frt_next_sequence next_runtime) (frt_log next_runtime).
Proof.
  intros runtime request next_runtime response [Hnodup Hall] Hrun.
  unfold run_filed_request in Hrun.
  destruct (apply_filed_op (frt_vfs runtime) (frq_op request)) as [next_vfs op_response].
  inversion Hrun; subst; clear Hrun.
  unfold sequence_log_wf.
  simpl.
  split.
  - constructor.
    + apply sequence_not_in_log_if_all_less.
      exact Hall.
    + exact Hnodup.
  - constructor.
    + apply Nat.lt_succ_diag_r.
    + eapply Forall_impl; [| exact Hall].
      intros entry Hlt.
      apply Nat.lt_lt_succ_r.
      exact Hlt.
Qed.

Theorem run_filed_request_preserves_runtime_wf_if_op_preserves :
  forall runtime request next_runtime response,
    filed_runtime_wf runtime ->
    filed_op_preserves_wf (frq_op request) ->
    run_filed_request runtime request = (next_runtime, response) ->
    filed_runtime_wf next_runtime.
Proof.
  intros runtime request next_runtime response [Hvfs Hseq] Hop Hrun.
  unfold filed_runtime_wf.
  split.
  - unfold run_filed_request in Hrun.
    destruct (apply_filed_op (frt_vfs runtime) (frq_op request)) as [next_vfs op_response] eqn:Happly.
    inversion Hrun; subst; clear Hrun.
    eapply Hop; eauto.
  - eapply run_filed_request_preserves_sequence_log_wf; eauto.
Qed.
