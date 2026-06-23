From Stdlib Require Import Bool.Bool Lists.List ZArith.ZArith.
From Pacha.Filed Require Import FiledTypes VfsModel ExecModel.

Import ListNotations.
Open Scope Z_scope.

Theorem invalid_elf_magic_fails :
  forall request image,
    eh_magic_ok (ei_header image) = false ->
    ex_status (build_exec_plan request image) = FiledErrBadFormat.
Proof.
  intros request image Hmagic.
  unfold build_exec_plan, build_exec_plan_with_handles, validate_elf_header.
  rewrite Hmagic.
  reflexivity.
Qed.

Theorem no_load_segments_fails :
  forall request image,
    validate_elf_header image = true ->
    collect_load_segments image (ei_phdrs image) = Some [] ->
    ex_status (build_exec_plan request image) = FiledErrInvalidImage.
Proof.
  intros request image Hheader Hsegments.
  unfold build_exec_plan, build_exec_plan_with_handles.
  rewrite Hheader.
  unfold build_mapping_plan.
  rewrite Hsegments.
  reflexivity.
Qed.

Theorem cloexec_fd_is_not_inherited :
  forall request fd,
    In fd (er_cloexec_fds request) ->
    ~ In fd (build_fd_inherit_plan request).
Proof.
  intros request fd Hin.
  unfold build_fd_inherit_plan, fd_inherited.
  intro Hfiltered.
  apply filter_In in Hfiltered as [_ Hkept].
  apply Bool.negb_true_iff in Hkept.
  assert (Hexists : existsb (Nat.eqb fd) (er_cloexec_fds request) = true).
  {
    rewrite existsb_exists.
    exists fd.
    split; [assumption | apply Nat.eqb_refl].
  }
  rewrite Hexists in Hkept.
  discriminate Hkept.
Qed.

Theorem cloexec_handle_is_not_inheritable_handle :
  forall handles handle,
    In handle handles ->
    handle_has_cloexec handle = true ->
    ~ In handle (filter handle_inheritable handles).
Proof.
  intros handles handle _ Hcloexec Hinfilter.
  apply filter_In in Hinfilter as [_ Hkeep].
  unfold handle_inheritable in Hkeep.
  rewrite Hcloexec in Hkeep.
  discriminate Hkeep.
Qed.

Theorem non_cloexec_handle_is_inherited :
  forall handles handle,
    In handle handles ->
    handle_has_cloexec handle = false ->
    In (vh_id handle) (build_handle_inherit_plan handles).
Proof.
  intros handles handle Hin Hcloexec.
  unfold build_handle_inherit_plan.
  apply in_map.
  apply filter_In.
  split; [exact Hin |].
  unfold handle_inheritable.
  rewrite Hcloexec.
  reflexivity.
Qed.

Theorem stack_plan_keeps_argv_env :
  forall request image,
    esp_argv (build_stack_plan_for_image request image) = er_argv request /\
    esp_env (build_stack_plan_for_image request image) = er_env request.
Proof.
  intros request image.
  split; reflexivity.
Qed.

Theorem stack_plan_argc_envc_fields :
  forall request image,
    esp_argc (build_stack_plan_for_image request image) = er_argc request /\
    esp_envc (build_stack_plan_for_image request image) = er_envc request.
Proof.
  intros request image.
  split; reflexivity.
Qed.

Theorem stack_plan_counts_match_request_when_wf :
  forall request image,
    exec_request_wf request ->
    esp_argc (build_stack_plan_for_image request image) =
      List.length (esp_argv (build_stack_plan_for_image request image)) /\
    esp_envc (build_stack_plan_for_image request image) =
      List.length (esp_env (build_stack_plan_for_image request image)).
Proof.
  intros request image [Hargc Henvc].
  unfold build_stack_plan_for_image.
  simpl.
  split; assumption.
Qed.

Theorem auxv_plan_contains_pagesz :
  forall request image,
    auxv_entry_with_key AuxvPagesz (build_auxv_plan request image) = true.
Proof.
  intros request image.
  unfold build_auxv_plan, auxv_entry_with_key.
  simpl.
  reflexivity.
Qed.

Theorem auxv_plan_contains_entry :
  forall request image,
    auxv_entry_with_key AuxvEntry (build_auxv_plan request image) = true.
Proof.
  intros request image.
  unfold build_auxv_plan, auxv_entry_with_key.
  simpl.
  reflexivity.
Qed.

Theorem bootstrap_fd_sets_auxv :
  forall request image fd,
    er_bootstrap_fd request = Some fd ->
    auxv_entry_with_key
      AuxvBootstrapFd
      (esp_auxv (build_stack_plan_for_image request image)) = true.
Proof.
  intros request image fd Hfd.
  unfold build_stack_plan_for_image, build_auxv_plan, bootstrap_auxv.
  rewrite Hfd.
  simpl.
  reflexivity.
Qed.

Theorem no_bootstrap_fd_omits_bootstrap_auxv :
  forall request image,
    er_bootstrap_fd request = None ->
    auxv_entry_with_key
      AuxvBootstrapFd
      (esp_auxv (build_stack_plan_for_image request image)) = false.
Proof.
  intros request image Hfd.
  unfold build_stack_plan_for_image, build_auxv_plan, bootstrap_auxv.
  rewrite Hfd.
  simpl.
  reflexivity.
Qed.

Theorem exec_plan_with_handles_keeps_inherited_handles :
  forall request image handles bias mappings plan,
    validate_elf_header image = true ->
    build_mapping_plan request image = Some (bias, mappings) ->
    build_exec_plan_with_handles request image handles =
      {|
        ex_status := FiledOk;
        ex_plan := Some plan;
      |} ->
    ep_inherited_handles plan = build_handle_inherit_plan handles.
Proof.
  intros request image handles bias mappings plan Hvalid Hmapping Hplan.
  unfold build_exec_plan_with_handles in Hplan.
  rewrite Hvalid in Hplan.
  rewrite Hmapping in Hplan.
  inversion Hplan; subst; clear Hplan.
  reflexivity.
Qed.

Example bootstrap_fd_sets_stack_flag :
  forall request fd,
    er_bootstrap_fd request = Some fd ->
    esp_has_bootstrap (build_stack_plan request) = true.
Proof.
  intros request fd Hfd.
  unfold build_stack_plan, build_stack_plan_for_image.
  cbn [ei_header].
  rewrite Hfd.
  reflexivity.
Qed.
