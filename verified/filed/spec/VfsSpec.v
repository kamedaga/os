From Stdlib Require Import Bool.Bool Lia Lists.List Strings.String ZArith.ZArith.
From Pacha.Filed Require Import FiledTypes VfsModel.

Import ListNotations.
Open Scope string_scope.
Open Scope Z_scope.

Definition vfs_operation_preserves
    {A : Type}
    (operation : vfs_state -> vfs_result A)
  : Prop :=
  forall state result_state value,
    well_formed_state state ->
    operation state =
      {|
        vr_status := FiledOk;
        vr_state := result_state;
        vr_value := Some value;
        vr_decision := no_vfs_decision;
      |} ->
    well_formed_state result_state.

Theorem empty_vfs_state_has_no_mounts :
  vs_mounts empty_vfs_state = [].
Proof.
  reflexivity.
Qed.

Theorem mount_root_returns_ok :
  forall state kind backend root_object,
    vr_status (mount_root state kind backend root_object) = FiledOk.
Proof.
  intros.
  reflexivity.
Qed.

Theorem path_walk_missing_root :
  forall state components,
    state_find_vnode state invalid_id = None ->
    vr_status (path_walk state invalid_id components) = FiledErrNotFound.
Proof.
  intros state components Hmissing.
  unfold path_walk, path_walk_with, path_walk_context.
  cbn [path_start_vnode_id pr_start pr_root pr_cwd].
  rewrite Hmissing.
  reflexivity.
Qed.

Theorem path_walk_budget_exhausted :
  forall state current component rest follow_final,
    vr_status
      (path_walk_from_budget
        0
        state
        current
        (component :: rest)
        follow_final) = FiledErrLoop.
Proof.
  reflexivity.
Qed.

Theorem path_walk_context_root_empty :
  forall state root cwd root_node follow_final,
    state_find_vnode state root = Some root_node ->
    vr_value
      (path_walk_context
        state
        {|
          pr_root := root;
          pr_cwd := cwd;
          pr_start := PathStartRoot;
          pr_follow_final_symlink := follow_final;
          pr_budget := 0%nat;
        |}
        []) = Some root_node.
Proof.
  intros state root cwd root_node follow_final Hroot.
  unfold path_walk_context.
  cbn [path_start_vnode_id pr_start pr_root pr_cwd].
  rewrite Hroot.
  reflexivity.
Qed.

Theorem path_walk_context_cwd_empty :
  forall state root cwd cwd_node follow_final,
    state_find_vnode state cwd = Some cwd_node ->
    vr_value
      (path_walk_context
        state
        {|
          pr_root := root;
          pr_cwd := cwd;
          pr_start := PathStartCwd;
          pr_follow_final_symlink := follow_final;
          pr_budget := 0%nat;
        |}
        []) = Some cwd_node.
Proof.
  intros state root cwd cwd_node follow_final Hcwd.
  unfold path_walk_context.
  cbn [path_start_vnode_id pr_start pr_root pr_cwd].
  rewrite Hcwd.
  reflexivity.
Qed.

Theorem path_walk_dotdot_at_mount_root_stays :
  forall state current follow_final,
    vnode_is_mount_root state current = true ->
    vr_value
      (path_walk_from_budget
        1
        state
        current
        [".."]
        follow_final) = Some current.
Proof.
  intros state current follow_final Hmount_root.
  unfold path_walk_from_budget.
  simpl.
  unfold parent_or_self.
  rewrite Hmount_root.
  reflexivity.
Qed.

Theorem path_walk_final_symlink_no_follow_returns_symlink :
  forall state directory name child,
    string_eqb name "." = false ->
    string_eqb name ".." = false ->
    vnode_is_directory directory = true ->
    lookup_child state directory name = Some child ->
    vnode_is_symlink child = true ->
    vr_value
      (path_walk_from_budget
        1
        state
        directory
        [name]
        false) = Some child.
Proof.
  intros state directory name child Hdot Hdotdot Hdir Hchild Hsymlink.
  unfold path_walk_from_budget.
  rewrite Hdot.
  rewrite Hdotdot.
  rewrite Hdir.
  rewrite Hchild.
  rewrite Hsymlink.
  reflexivity.
Qed.

Theorem path_walk_follow_symlink_consumes_budget :
  forall state directory name child target,
    string_eqb name "." = false ->
    string_eqb name ".." = false ->
    vnode_is_directory directory = true ->
    lookup_child state directory name = Some child ->
    vnode_is_symlink child = true ->
    vn_symlink_target child = Some target ->
    target <> [] ->
    vr_status
      (path_walk_from_budget
        1
        state
        directory
        [name]
        true) = FiledErrLoop.
Proof.
  intros state directory name child target Hdot Hdotdot Hdir Hchild Hsymlink Htarget Hnonempty.
  unfold path_walk_from_budget.
  rewrite Hdot.
  rewrite Hdotdot.
  rewrite Hdir.
  rewrite Hchild.
  rewrite Hsymlink.
  rewrite Htarget.
  destruct target as [| head rest].
  - contradiction Hnonempty.
    reflexivity.
  - reflexivity.
Qed.

Theorem openat_is_openat_with_empty_flags :
  forall state root components rights,
    openat state root components rights =
    openat_with_flags state root components rights [].
Proof.
  reflexivity.
Qed.

Theorem openat_with_flags_denies_without_lookup_right :
  forall state root components rights flags,
    has_all_rights [VfsRightLookup] rights = false ->
    vr_status (openat_with_flags state root components rights flags) = FiledErrDenied.
Proof.
  intros state root components rights flags Hrights.
  unfold openat_with_flags.
  rewrite Hrights.
  reflexivity.
Qed.

Theorem openat_create_exclusive_existing_fails :
  forall state root components rights flags walked_state node,
    has_all_rights [VfsRightLookup] rights = true ->
    path_walk_with
      state
      root
      components
      (negb (has_open_flag VfsOpenNoFollow flags)) =
      {|
        vr_status := FiledOk;
        vr_state := walked_state;
        vr_value := Some node;
        vr_decision := no_vfs_decision;
      |} ->
    has_open_flag VfsOpenCreate flags = true ->
    has_open_flag VfsOpenExclusive flags = true ->
    vr_status (openat_with_flags state root components rights flags) = FiledErrExists.
Proof.
  intros state root components rights flags walked_state node
    Hrights Hwalk Hcreate Hexcl.
  unfold openat_with_flags.
  rewrite Hrights.
  rewrite Hwalk.
  rewrite Hcreate.
  rewrite Hexcl.
  reflexivity.
Qed.

Theorem openat_missing_without_create_fails :
  forall state root components rights flags walked_state,
    has_all_rights [VfsRightLookup] rights = true ->
    path_walk_with
      state
      root
      components
      (negb (has_open_flag VfsOpenNoFollow flags)) =
      {|
        vr_status := FiledErrNotFound;
        vr_state := walked_state;
        vr_value := None;
        vr_decision := no_vfs_decision;
      |} ->
    has_open_flag VfsOpenCreate flags = false ->
    vr_status (openat_with_flags state root components rights flags) = FiledErrNotFound.
Proof.
  intros state root components rights flags walked_state Hrights Hwalk Hcreate.
  unfold openat_with_flags.
  rewrite Hrights.
  rewrite Hwalk.
  rewrite Hcreate.
  reflexivity.
Qed.

Theorem openat_no_follow_rejects_final_symlink :
  forall state root components rights flags walked_state node,
    has_all_rights [VfsRightLookup] rights = true ->
    has_open_flag VfsOpenNoFollow flags = true ->
    andb (has_open_flag VfsOpenCreate flags)
         (has_open_flag VfsOpenExclusive flags) = false ->
    path_walk_with state root components false =
      {|
        vr_status := FiledOk;
        vr_state := walked_state;
        vr_value := Some node;
        vr_decision := no_vfs_decision;
      |} ->
    vn_kind node = VnodeSymlink ->
    vr_status (openat_with_flags state root components rights flags) = FiledErrLoop.
Proof.
  intros state root components rights flags walked_state node
    Hrights Hnofollow Hnot_excl Hwalk Hkind.
  unfold openat_with_flags.
  rewrite Hrights.
  rewrite Hnofollow.
  change (negb true) with false.
  rewrite Hwalk.
  rewrite Hnot_excl.
  unfold open_existing, open_existing_status.
  rewrite Hnofollow.
  rewrite Hkind.
  reflexivity.
Qed.

Theorem create_regular_child_requires_create_right :
  forall state parent name rights flags,
    has_right VfsRightCreate rights = false ->
    vr_status (create_regular_child state parent name rights flags) = FiledErrDenied.
Proof.
  intros state parent name rights flags Hcreate.
  unfold create_regular_child.
  rewrite Hcreate.
  reflexivity.
Qed.

Theorem create_regular_child_rejects_directory_flag :
  forall state parent name rights flags,
    has_right VfsRightCreate rights = true ->
    has_open_flag VfsOpenDirectory flags = true ->
    vr_status (create_regular_child state parent name rights flags) = FiledErrInvalid.
Proof.
  intros state parent name rights flags Hcreate Hdir.
  unfold create_regular_child.
  rewrite Hcreate.
  rewrite Hdir.
  reflexivity.
Qed.

Theorem open_existing_status_no_follow_symlink :
  forall node rights flags,
    vn_kind node = VnodeSymlink ->
    has_open_flag VfsOpenNoFollow flags = true ->
    open_existing_status node rights flags = FiledErrLoop.
Proof.
  intros node rights flags Hkind Hnofollow.
  unfold open_existing_status.
  rewrite Hnofollow.
  rewrite Hkind.
  reflexivity.
Qed.

Theorem open_existing_status_directory_flag_rejects_non_directory :
  forall node rights flags,
    has_open_flag VfsOpenNoFollow flags = false ->
    has_open_flag VfsOpenDirectory flags = true ->
    vnode_is_directory node = false ->
    open_existing_status node rights flags = FiledErrNotDir.
Proof.
  intros node rights flags Hnofollow Hdir Hnotdir.
  unfold open_existing_status.
  rewrite Hnofollow.
  rewrite Hdir.
  rewrite Hnotdir.
  reflexivity.
Qed.

Theorem open_existing_status_truncate_requires_write :
  forall node rights flags,
    has_open_flag VfsOpenNoFollow flags = false ->
    has_open_flag VfsOpenDirectory flags = false ->
    has_open_flag VfsOpenTruncate flags = true ->
    has_right VfsRightWrite rights = false ->
    open_existing_status node rights flags = FiledErrDenied.
Proof.
  intros node rights flags Hnofollow Hdir Htrunc Hwrite.
  unfold open_existing_status.
  rewrite Hnofollow.
  rewrite Hdir.
  rewrite Htrunc.
  rewrite Hwrite.
  reflexivity.
Qed.

Theorem open_file_description_cloexec_sets_fd_flag :
  forall state node rights flags handle_id handle,
    has_open_flag VfsOpenCloseOnExec flags = true ->
    vr_value (open_file_description state node rights flags) = Some handle_id ->
    state_find_handle
      (vr_state (open_file_description state node rights flags))
      handle_id = Some handle ->
    has_fd_flag VfsFdCloseOnExec (vh_fd_flags handle) = true.
Proof.
  intros state node rights flags handle_id handle Hcloexec Hvalue Hhandle.
  unfold open_file_description in Hvalue.
  inversion Hvalue; subst; clear Hvalue.
  unfold open_file_description in Hhandle.
  unfold state_find_handle in Hhandle.
  simpl in Hhandle.
  rewrite Nat.eqb_refl in Hhandle.
  inversion Hhandle; subst; clear Hhandle.
  simpl.
  unfold fd_flags_from_open.
  rewrite Hcloexec.
  reflexivity.
Qed.

Theorem open_file_description_append_sets_status_flag :
  forall state node rights flags file,
    has_open_flag VfsOpenAppend flags = true ->
    state_find_file
      (vr_state (open_file_description state node rights flags))
      (vs_next_file_id state) = Some file ->
    has_file_status_flag VfsFileAppend (vf_status_flags file) = true.
Proof.
  intros state node rights flags file Happend Hfile.
  unfold open_file_description in Hfile.
  unfold state_find_file in Hfile.
  simpl in Hfile.
  rewrite Nat.eqb_refl in Hfile.
  inversion Hfile; subst; clear Hfile.
  simpl.
  unfold file_status_flags_from_open.
  rewrite Happend.
  reflexivity.
Qed.

Theorem open_file_description_truncate_requests_backend_truncate :
  forall state node rights flags,
    has_open_flag VfsOpenTruncate flags = true ->
    vd_kind (vr_decision (open_file_description state node rights flags)) =
      VfsDecisionBackendTruncate /\
    vd_vnode_id (vr_decision (open_file_description state node rights flags)) =
      vn_id node.
Proof.
  intros state node rights flags Htrunc.
  unfold open_file_description, open_truncate_decision.
  rewrite Htrunc.
  split; reflexivity.
Qed.

Theorem open_file_description_without_truncate_has_no_decision :
  forall state node rights flags,
    has_open_flag VfsOpenTruncate flags = false ->
    vr_decision (open_file_description state node rights flags) = no_vfs_decision.
Proof.
  intros state node rights flags Htrunc.
  unfold open_file_description, open_truncate_decision.
  rewrite Htrunc.
  reflexivity.
Qed.

Theorem mkdirat_requires_create_right :
  forall state root components rights,
    has_right VfsRightCreate rights = false ->
    vr_status (mkdirat state root components rights) = FiledErrDenied.
Proof.
  intros state root components rights Hright.
  unfold mkdirat.
  rewrite Hright.
  reflexivity.
Qed.

Theorem unlinkat_requires_remove_right :
  forall state root components rights,
    has_right VfsRightRemove rights = false ->
    vr_status (unlinkat state root components rights) = FiledErrDenied.
Proof.
  intros state root components rights Hright.
  unfold unlinkat.
  rewrite Hright.
  reflexivity.
Qed.

Theorem rmdir_requires_remove_right :
  forall state root components rights,
    has_right VfsRightRemove rights = false ->
    vr_status (rmdir state root components rights) = FiledErrDenied.
Proof.
  intros state root components rights Hright.
  unfold rmdir.
  rewrite Hright.
  reflexivity.
Qed.

Theorem renameat_requires_rename_right :
  forall state root source target rights,
    has_right VfsRightRename rights = false ->
    vr_status (renameat state root source target rights) = FiledErrDenied.
Proof.
  intros state root source target rights Hright.
  unfold renameat.
  rewrite Hright.
  reflexivity.
Qed.

Theorem lookup_child_in_ignores_unlinked_matching_head :
  forall mount parent name node,
    vn_linked node = false ->
    lookup_child_in [node] mount parent name = None.
Proof.
  intros mount parent name node Hunlinked.
  simpl.
  rewrite Hunlinked.
  destruct (vn_parent node) as [actual_parent|].
  - destruct (Nat.eqb (vn_mount_id node) mount);
    destruct (Nat.eqb actual_parent parent);
    destruct (string_eqb (vn_name node) name);
    reflexivity.
  - destruct (Nat.eqb (vn_mount_id node) mount);
    destruct (string_eqb (vn_name node) name);
    reflexivity.
Qed.

Lemma find_vnode_mark_vnode_unlinked_same :
  forall nodes id node,
    find_vnode nodes id = Some node ->
    find_vnode (map (mark_vnode_unlinked_entry id) nodes) id =
      Some (vnode_with_linked node false).
Proof.
  induction nodes as [| head rest IH]; intros id node Hfind.
  - simpl in Hfind.
    discriminate Hfind.
  - simpl in Hfind.
    destruct (Nat.eqb (vn_id head) id) eqn:Heq.
    + inversion Hfind; subst; clear Hfind.
      simpl.
      unfold mark_vnode_unlinked_entry.
      rewrite Heq.
      simpl.
      rewrite Heq.
      reflexivity.
    + simpl.
      unfold mark_vnode_unlinked_entry.
      rewrite Heq.
      simpl.
      rewrite Heq.
      apply IH.
      exact Hfind.
Qed.

Theorem mark_vnode_unlinked_keeps_vnode_object :
  forall state id node,
    state_find_vnode state id = Some node ->
    state_find_vnode (mark_vnode_unlinked state id) id =
      Some (vnode_with_linked node false).
Proof.
  intros state id node Hfind.
  unfold state_find_vnode, mark_vnode_unlinked, set_vnodes.
  simpl.
  apply find_vnode_mark_vnode_unlinked_same.
  exact Hfind.
Qed.

Lemma find_vnode_rename_vnode_same :
  forall nodes id node parent name,
    find_vnode nodes id = Some node ->
    find_vnode (map (rename_vnode_entry id parent name) nodes) id =
      Some (vnode_with_parent_name node parent name).
Proof.
  induction nodes as [| head rest IH]; intros id node parent name Hfind.
  - simpl in Hfind.
    discriminate Hfind.
  - simpl in Hfind.
    destruct (Nat.eqb (vn_id head) id) eqn:Heq.
    + inversion Hfind; subst; clear Hfind.
      simpl.
      unfold rename_vnode_entry.
      rewrite Heq.
      simpl.
      rewrite Heq.
      reflexivity.
    + simpl.
      unfold rename_vnode_entry.
      rewrite Heq.
      simpl.
      rewrite Heq.
      apply IH.
      exact Hfind.
Qed.

Theorem rename_vnode_moves_source :
  forall state id node parent name,
    state_find_vnode state id = Some node ->
    state_find_vnode (rename_vnode state id parent name) id =
      Some (vnode_with_parent_name node parent name).
Proof.
  intros state id node parent name Hfind.
  unfold state_find_vnode, rename_vnode, set_vnodes.
  simpl.
  apply find_vnode_rename_vnode_same.
  exact Hfind.
Qed.

Theorem rename_commit_cross_mount_rejected :
  forall state source target_parent target_name target_existing,
    Nat.eqb (vn_mount_id source) (vn_mount_id target_parent) = false ->
    vr_status
      (rename_commit state source target_parent target_name target_existing) =
      FiledErrCrossMount.
Proof.
  intros state source target_parent target_name target_existing Hcross.
  unfold rename_commit.
  rewrite Hcross.
  reflexivity.
Qed.

Theorem rename_commit_same_source_target_noop :
  forall state source target_parent target_name target,
    Nat.eqb (vn_mount_id source) (vn_mount_id target_parent) = true ->
    Nat.eqb (vn_id source) (vn_id target) = true ->
    vr_state (rename_commit state source target_parent target_name (Some target)) = state.
Proof.
  intros state source target_parent target_name target Hmount Hid.
  unfold rename_commit.
  rewrite Hmount.
  rewrite Hid.
  reflexivity.
Qed.

Theorem rename_commit_rejects_nonempty_target_directory :
  forall state source target_parent target_name target,
    Nat.eqb (vn_mount_id source) (vn_mount_id target_parent) = true ->
    Nat.eqb (vn_id source) (vn_id target) = false ->
    vnode_is_directory target = true ->
    directory_empty state target = false ->
    vr_status (rename_commit state source target_parent target_name (Some target)) =
      FiledErrNotEmpty.
Proof.
  intros state source target_parent target_name target Hmount Hid Hdir Hempty.
  unfold rename_commit.
  rewrite Hmount.
  rewrite Hid.
  rewrite Hdir.
  rewrite Hempty.
  reflexivity.
Qed.

Theorem rename_commit_missing_target_moves_source :
  forall state source target_parent target_name,
    Nat.eqb (vn_mount_id source) (vn_mount_id target_parent) = true ->
    state_find_vnode state (vn_id source) = Some source ->
    state_find_vnode
      (vr_state (rename_commit state source target_parent target_name None))
      (vn_id source) =
      Some (vnode_with_parent_name source target_parent target_name).
Proof.
  intros state source target_parent target_name Hmount Hfind.
  unfold rename_commit.
  rewrite Hmount.
  apply rename_vnode_moves_source.
  exact Hfind.
Qed.

Theorem close_missing_handle :
  forall state handle,
    state_find_handle state handle = None ->
    vr_status (close state handle) = FiledErrInvalid.
Proof.
  intros state handle Hmissing.
  unfold close.
  rewrite Hmissing.
  reflexivity.
Qed.

Theorem dup_attenuate_missing_handle :
  forall state handle rights fd_flags,
    state_find_handle state handle = None ->
    vr_status (dup_attenuate state handle rights fd_flags) = FiledErrInvalid.
Proof.
  intros state handle rights fd_flags Hmissing.
  unfold dup_attenuate.
  rewrite Hmissing.
  reflexivity.
Qed.

Theorem dup_attenuate_default_missing_handle :
  forall state handle rights,
    state_find_handle state handle = None ->
    vr_status (dup_attenuate_default state handle rights) = FiledErrInvalid.
Proof.
  intros state handle rights Hmissing.
  unfold dup_attenuate_default, dup_attenuate.
  rewrite Hmissing.
  reflexivity.
Qed.

Theorem dup_attenuate_shares_open_file_description :
  forall state handle rights fd_flags original new_handle_id new_handle,
    state_find_handle state handle = Some original ->
    has_all_rights rights (vh_rights original) = true ->
    vr_value (dup_attenuate state handle rights fd_flags) = Some new_handle_id ->
    state_find_handle
      (vr_state (dup_attenuate state handle rights fd_flags))
      new_handle_id = Some new_handle ->
    vh_target new_handle = vh_target original.
Proof.
  intros state handle rights fd_flags original new_handle_id new_handle
    Hfind Hrights Hvalue Hnew.
  unfold dup_attenuate in Hvalue.
  rewrite Hfind in Hvalue.
  rewrite Hrights in Hvalue.
  destruct (dup_base_state state (vh_target original)) as [base_state|] eqn:Hbase;
    try discriminate.
  inversion Hvalue; subst; clear Hvalue.
  unfold dup_attenuate in Hnew.
  rewrite Hfind in Hnew.
  rewrite Hrights in Hnew.
  rewrite Hbase in Hnew.
  unfold state_find_handle in Hnew.
  simpl in Hnew.
  rewrite Nat.eqb_refl in Hnew.
  inversion Hnew; subst.
  reflexivity.
Qed.

Theorem dup_attenuate_sets_fd_flags :
  forall state handle rights fd_flags original new_handle_id new_handle,
    state_find_handle state handle = Some original ->
    has_all_rights rights (vh_rights original) = true ->
    vr_value (dup_attenuate state handle rights fd_flags) = Some new_handle_id ->
    state_find_handle
      (vr_state (dup_attenuate state handle rights fd_flags))
      new_handle_id = Some new_handle ->
    vh_fd_flags new_handle = fd_flags.
Proof.
  intros state handle rights fd_flags original new_handle_id new_handle
    Hfind Hrights Hvalue Hnew.
  unfold dup_attenuate in Hvalue.
  rewrite Hfind in Hvalue.
  rewrite Hrights in Hvalue.
  destruct (dup_base_state state (vh_target original)) as [base_state|] eqn:Hbase;
    try discriminate.
  inversion Hvalue; subst; clear Hvalue.
  unfold dup_attenuate in Hnew.
  rewrite Hfind in Hnew.
  rewrite Hrights in Hnew.
  rewrite Hbase in Hnew.
  unfold state_find_handle in Hnew.
  simpl in Hnew.
  rewrite Nat.eqb_refl in Hnew.
  inversion Hnew; subst.
  reflexivity.
Qed.

Theorem dup_attenuate_rejects_missing_open_file_description :
  forall state handle rights fd_flags original file,
    state_find_handle state handle = Some original ->
    vh_target original = HandleFile file ->
    state_find_file state file = None ->
    has_all_rights rights (vh_rights original) = true ->
    vr_status (dup_attenuate state handle rights fd_flags) = FiledErrInvalid.
Proof.
  intros state handle rights fd_flags original file Hfind Htarget Hfile Hrights.
  unfold dup_attenuate.
  rewrite Hfind.
  rewrite Hrights.
  unfold dup_base_state.
  rewrite Htarget.
  rewrite Hfile.
  reflexivity.
Qed.

Theorem cloexec_handle_not_inheritable :
  forall state handle,
    In handle (vs_handles state) ->
    handle_has_cloexec handle = true ->
    ~ In handle (inheritable_handles state).
Proof.
  intros state handle _ Hcloexec Hinherited.
  unfold inheritable_handles in Hinherited.
  apply filter_In in Hinherited as [_ Hkeep].
  unfold handle_inheritable in Hkeep.
  rewrite Hcloexec in Hkeep.
  discriminate Hkeep.
Qed.

Lemma find_file_set_file_offset_same :
  forall files id file offset,
    find_file files id = Some file ->
    find_file (map (set_file_offset_entry id offset) files) id =
      Some (file_with_offset file offset).
Proof.
  induction files as [| head rest IH]; intros id file offset Hfind.
  - simpl in Hfind.
    discriminate Hfind.
  - simpl in Hfind.
    destruct (Nat.eqb (vf_id head) id) eqn:Heq.
    + inversion Hfind; subst; clear Hfind.
      simpl.
      unfold set_file_offset_entry.
      rewrite Heq.
      simpl.
      rewrite Heq.
      reflexivity.
    + simpl.
      unfold set_file_offset_entry.
      rewrite Heq.
      simpl.
      rewrite Heq.
      apply IH.
      exact Hfind.
Qed.

Theorem read_prepare_missing_handle :
  forall state handle length,
    state_find_handle state handle = None ->
    vr_status (read_prepare state handle length) = FiledErrInvalid.
Proof.
  intros state handle length Hmissing.
  unfold read_prepare.
  rewrite Hmissing.
  reflexivity.
Qed.

Theorem read_prepare_uses_current_open_file_offset :
  forall state handle length vhandle file_id file node,
    state_find_handle state handle = Some vhandle ->
    vh_target vhandle = HandleFile file_id ->
    state_find_file state file_id = Some file ->
    state_find_vnode state (vf_vnode_id file) = Some node ->
    has_right VfsRightRead (vh_rights vhandle) = true ->
    z_nonnegative length = true ->
    vnode_is_regular node = true ->
    vd_offset (vr_decision (read_prepare state handle length)) = vf_offset file.
Proof.
  intros state handle length vhandle file_id file node
    Hhandle Htarget Hfile Hnode Hread Hlength Hregular.
  unfold read_prepare.
  rewrite Hhandle.
  rewrite Hread.
  rewrite Hlength.
  rewrite Htarget.
  rewrite Hfile.
  rewrite Hnode.
  rewrite Hregular.
  reflexivity.
Qed.

Theorem read_prepare_advances_shared_open_file_offset :
  forall state handle length vhandle file_id file node,
    state_find_handle state handle = Some vhandle ->
    vh_target vhandle = HandleFile file_id ->
    state_find_file state file_id = Some file ->
    state_find_vnode state (vf_vnode_id file) = Some node ->
    has_right VfsRightRead (vh_rights vhandle) = true ->
    z_nonnegative length = true ->
    vnode_is_regular node = true ->
    state_find_file
      (vr_state (read_prepare state handle length))
      file_id =
      Some (file_with_offset file (vf_offset file + length)).
Proof.
  intros state handle length vhandle file_id file node
    Hhandle Htarget Hfile Hnode Hread Hlength Hregular.
  unfold read_prepare.
  rewrite Hhandle.
  rewrite Hread.
  rewrite Hlength.
  rewrite Htarget.
  rewrite Hfile.
  rewrite Hnode.
  rewrite Hregular.
  unfold state_find_file, set_file_offset.
  simpl.
  apply find_file_set_file_offset_same.
  exact Hfile.
Qed.

Theorem pread_prepare_missing_handle :
  forall state handle offset length,
    state_find_handle state handle = None ->
    vr_status (pread_prepare state handle offset length) = FiledErrInvalid.
Proof.
  intros state handle offset length Hmissing.
  unfold pread_prepare.
  rewrite Hmissing.
  reflexivity.
Qed.

Example mount_root_allocates_first_mount :
  vr_value (mount_root empty_vfs_state FsExt4 10%nat 20%nat) = Some 1%nat.
Proof.
  reflexivity.
Qed.
