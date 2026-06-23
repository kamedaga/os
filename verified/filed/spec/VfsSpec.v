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

Theorem create_prepare_requires_lookup_right :
  forall state root components rights flags,
    has_all_rights [VfsRightLookup] rights = false ->
    vr_status (create_prepare state root components rights flags) = FiledErrDenied.
Proof.
  intros state root components rights flags Hlookup.
  unfold create_prepare.
  rewrite Hlookup.
  reflexivity.
Qed.

Theorem create_prepare_requires_create_right :
  forall state root components rights flags,
    has_all_rights [VfsRightLookup] rights = true ->
    has_right VfsRightCreate rights = false ->
    vr_status (create_prepare state root components rights flags) = FiledErrDenied.
Proof.
  intros state root components rights flags Hlookup Hcreate.
  unfold create_prepare.
  rewrite Hlookup.
  rewrite Hcreate.
  reflexivity.
Qed.

Theorem create_prepare_rejects_empty_path :
  forall state root rights flags,
    has_all_rights [VfsRightLookup] rights = true ->
    has_right VfsRightCreate rights = true ->
    vr_status (create_prepare state root [] rights flags) = FiledErrInvalid.
Proof.
  intros state root rights flags Hlookup Hcreate.
  unfold create_prepare.
  rewrite Hlookup.
  rewrite Hcreate.
  reflexivity.
Qed.

Theorem create_prepare_rejects_dot :
  forall state root rights flags,
    has_all_rights [VfsRightLookup] rights = true ->
    has_right VfsRightCreate rights = true ->
    vr_status (create_prepare state root ["."%string] rights flags) = FiledErrInvalid.
Proof.
  intros state root rights flags Hlookup Hcreate.
  unfold create_prepare.
  rewrite Hlookup.
  rewrite Hcreate.
  reflexivity.
Qed.

Theorem create_prepare_rejects_dotdot :
  forall state root rights flags,
    has_all_rights [VfsRightLookup] rights = true ->
    has_right VfsRightCreate rights = true ->
    vr_status (create_prepare state root [".."%string] rights flags) = FiledErrInvalid.
Proof.
  intros state root rights flags Hlookup Hcreate.
  unfold create_prepare.
  rewrite Hlookup.
  rewrite Hcreate.
  reflexivity.
Qed.

Theorem create_prepare_parent_must_be_directory :
  forall state root components parent_components name rights flags walked_state parent,
    has_all_rights [VfsRightLookup] rights = true ->
    has_right VfsRightCreate rights = true ->
    split_parent_components components = Some (parent_components, name) ->
    valid_new_child_name name = true ->
    path_walk_with state root parent_components true =
      {|
        vr_status := FiledOk;
        vr_state := walked_state;
        vr_value := Some parent;
        vr_decision := no_vfs_decision;
      |} ->
    vnode_is_directory parent = false ->
    vr_status
      (create_prepare state root components rights flags) =
      FiledErrNotDir.
Proof.
  intros state root components parent_components name rights flags walked_state parent
    Hlookup Hcreate Hsplit Hname Hwalk Hdir.
  unfold create_prepare.
  rewrite Hlookup.
  rewrite Hcreate.
  rewrite Hsplit.
  rewrite Hname.
  rewrite Hwalk.
  rewrite Hdir.
  reflexivity.
Qed.

Theorem create_prepare_existing_without_excl_opens_existing :
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
    andb (has_open_flag VfsOpenCreate flags)
         (has_open_flag VfsOpenExclusive flags) = false ->
    vr_status (openat_create_prepare state root components rights flags) = FiledOk /\
    vr_decision (openat_create_prepare state root components rights flags) =
      no_vfs_decision /\
    openat_with_flags state root components rights flags =
      open_existing walked_state node rights flags.
Proof.
  intros state root components rights flags walked_state node
    Hlookup Hwalk Hnot_excl.
  unfold openat_create_prepare, openat_with_flags.
  rewrite Hlookup.
  rewrite Hwalk.
  rewrite Hnot_excl.
  repeat split; reflexivity.
Qed.

Theorem create_prepare_existing_with_excl_fails :
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
    vr_status (openat_create_prepare state root components rights flags) =
      FiledErrExists.
Proof.
  intros state root components rights flags walked_state node
    Hlookup Hwalk Hcreate Hexcl.
  unfold openat_create_prepare.
  rewrite Hlookup.
  rewrite Hwalk.
  rewrite Hcreate.
  rewrite Hexcl.
  reflexivity.
Qed.

Theorem create_prepare_missing_returns_backend_create_decision :
  forall state root components parent_components name rights flags walked_state parent,
    has_all_rights [VfsRightLookup] rights = true ->
    has_right VfsRightCreate rights = true ->
    split_parent_components components = Some (parent_components, name) ->
    valid_new_child_name name = true ->
    path_walk_with state root parent_components true =
      {|
        vr_status := FiledOk;
        vr_state := walked_state;
        vr_value := Some parent;
        vr_decision := no_vfs_decision;
      |} ->
    vnode_is_directory parent = true ->
    lookup_child walked_state parent name = None ->
    vr_status
      (create_prepare state root components rights flags) =
      FiledOk /\
    vd_kind
      (vr_decision
        (create_prepare state root components rights flags)) =
      VfsDecisionBackendCreate /\
    vd_vnode_id
      (vr_decision
        (create_prepare state root components rights flags)) =
      vn_id parent /\
    vd_name
      (vr_decision
        (create_prepare state root components rights flags)) =
      Some name.
Proof.
  intros state root components parent_components name rights flags walked_state parent
    Hlookup Hcreate Hsplit Hname Hwalk Hdir Hchild.
  unfold create_prepare.
  rewrite Hlookup.
  rewrite Hcreate.
  rewrite Hsplit.
  rewrite Hname.
  rewrite Hwalk.
  rewrite Hdir.
  rewrite Hchild.
  repeat split; reflexivity.
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

Theorem create_commit_adds_linked_regular_vnode :
  forall state parent name backend_object rights flags,
    state_find_vnode
      (vr_state
        (create_commit state parent name backend_object rights flags))
      (vs_next_vnode_id state) =
      Some
        (mk_regular_vnode_with_backend
          (vs_next_vnode_id state)
          parent
          name
          backend_object).
Proof.
  intros state parent name backend_object rights flags.
  unfold create_commit, open_file_description, state_find_vnode.
  simpl.
  rewrite Nat.eqb_refl.
  reflexivity.
Qed.

Theorem create_commit_uses_backend_object :
  forall state parent name backend_object rights flags node,
    state_find_vnode
      (vr_state
        (create_commit state parent name backend_object rights flags))
      (vs_next_vnode_id state) = Some node ->
    vn_backend_object node = backend_object.
Proof.
  intros state parent name backend_object rights flags node Hnode.
  rewrite create_commit_adds_linked_regular_vnode in Hnode.
  inversion Hnode; subst; clear Hnode.
  reflexivity.
Qed.

Theorem create_commit_creates_open_file_description :
  forall state parent name backend_object rights flags,
    vr_value (create_commit state parent name backend_object rights flags) =
      Some (vs_next_handle_id state).
Proof.
  intros state parent name backend_object rights flags.
  unfold create_commit, open_file_description.
  reflexivity.
Qed.

Theorem create_commit_preserves_existing_handles :
  forall state parent name backend_object rights flags handle,
    In handle (vs_handles state) ->
    In handle
      (vs_handles
        (vr_state
          (create_commit state parent name backend_object rights flags))).
Proof.
  intros state parent name backend_object rights flags handle Hin.
  unfold create_commit, open_file_description.
  simpl.
  right.
  exact Hin.
Qed.

Theorem create_commit_preserves_linked_child_uniqueness :
  forall state parent name backend_object rights flags,
    linked_child_name_unique state ->
    (forall node,
      In node (vs_vnodes state) ->
      vn_linked node = true ->
      vn_mount_id node = vn_mount_id parent ->
      vn_parent node = Some (vn_id parent) ->
      vn_name node = name ->
      False) ->
    linked_child_name_unique
      (vr_state
        (create_commit state parent name backend_object rights flags)).
Proof.
  intros state parent name backend_object rights flags Hunique Habsent.
  unfold linked_child_name_unique in *.
  intros lhs rhs Hlhs Hrhs Hsame.
  unfold create_commit, open_file_description in Hlhs, Hrhs.
  simpl in Hlhs, Hrhs.
  destruct Hsame as [Hlhs_linked [Hrhs_linked [Hmount [Hparent Hname]]]].
  destruct Hlhs as [Hlhs_new | Hlhs_old];
  destruct Hrhs as [Hrhs_new | Hrhs_old].
  - subst lhs rhs.
    reflexivity.
  - subst lhs.
    exfalso.
    apply (Habsent rhs Hrhs_old Hrhs_linked).
    + symmetry.
      exact Hmount.
    + symmetry.
      exact Hparent.
    + symmetry.
      exact Hname.
  - subst rhs.
    exfalso.
    apply (Habsent lhs Hlhs_old Hlhs_linked).
    + exact Hmount.
    + exact Hparent.
    + exact Hname.
  - apply Hunique; try exact Hlhs_old; try exact Hrhs_old.
    repeat split; assumption.
Qed.

Lemma find_vnode_some_id :
  forall nodes id node,
    find_vnode nodes id = Some node ->
    vn_id node = id.
Proof.
  induction nodes as [| head rest IH]; intros id node Hfind.
  - discriminate Hfind.
  - simpl in Hfind.
    destruct (Nat.eqb (vn_id head) id) eqn:Heq.
    + inversion Hfind; subst; clear Hfind.
      apply Nat.eqb_eq.
      exact Heq.
    + apply IH.
      exact Hfind.
Qed.

Lemma find_vnode_some_in :
  forall nodes id node,
    find_vnode nodes id = Some node ->
    In node nodes.
Proof.
  induction nodes as [| head rest IH]; intros id node Hfind.
  - discriminate Hfind.
  - simpl in Hfind.
    destruct (Nat.eqb (vn_id head) id) eqn:Heq.
    + inversion Hfind; subst; clear Hfind.
      left.
      reflexivity.
    + right.
      apply (IH id).
      exact Hfind.
Qed.

Lemma find_vnode_cons_fresh :
  forall nodes new_node id,
    vn_id new_node <> id ->
    find_vnode (new_node :: nodes) id = find_vnode nodes id.
Proof.
  intros nodes new_node id Hfresh.
  simpl.
  destruct (Nat.eqb (vn_id new_node) id) eqn:Heq.
  - apply Nat.eqb_eq in Heq.
    contradiction.
  - reflexivity.
Qed.

Lemma find_file_some_id :
  forall files id file,
    find_file files id = Some file ->
    vf_id file = id.
Proof.
  induction files as [| head rest IH]; intros id file Hfind.
  - discriminate Hfind.
  - simpl in Hfind.
    destruct (Nat.eqb (vf_id head) id) eqn:Heq.
    + inversion Hfind; subst; clear Hfind.
      apply Nat.eqb_eq.
      exact Heq.
    + apply IH.
      exact Hfind.
Qed.

Lemma find_file_some_in :
  forall files id file,
    find_file files id = Some file ->
    In file files.
Proof.
  induction files as [| head rest IH]; intros id file Hfind.
  - discriminate Hfind.
  - simpl in Hfind.
    destruct (Nat.eqb (vf_id head) id) eqn:Heq.
    + inversion Hfind; subst; clear Hfind.
      left.
      reflexivity.
    + right.
      apply (IH id).
      exact Hfind.
Qed.

Lemma find_file_cons_fresh :
  forall files new_file id,
    vf_id new_file <> id ->
    find_file (new_file :: files) id = find_file files id.
Proof.
  intros files new_file id Hfresh.
  simpl.
  destruct (Nat.eqb (vf_id new_file) id) eqn:Heq.
  - apply Nat.eqb_eq in Heq.
    contradiction.
  - reflexivity.
Qed.

Lemma state_find_vnode_after_create_commit_old :
  forall state parent name backend_object rights flags id node,
    fresh_vnode_id state (vs_next_vnode_id state) ->
    state_find_vnode state id = Some node ->
    state_find_vnode
      (vr_state
        (create_commit state parent name backend_object rights flags))
      id = Some node.
Proof.
  intros state parent name backend_object rights flags id node Hfresh Hfind.
  unfold create_commit, open_file_description, state_find_vnode in *.
  simpl.
  destruct (Nat.eqb (vs_next_vnode_id state) id) eqn:Heq.
  - apply Nat.eqb_eq in Heq.
    pose proof (find_vnode_some_id (vs_vnodes state) id node Hfind) as Hid.
    pose proof (find_vnode_some_in (vs_vnodes state) id node Hfind) as Hin.
    exfalso.
    apply (Hfresh node Hin).
    rewrite Hid.
    symmetry.
    exact Heq.
  - exact Hfind.
Qed.

Lemma state_find_file_after_create_commit_old :
  forall state parent name backend_object rights flags id file,
    fresh_file_id state (vs_next_file_id state) ->
    state_find_file state id = Some file ->
    state_find_file
      (vr_state
        (create_commit state parent name backend_object rights flags))
      id = Some file.
Proof.
  intros state parent name backend_object rights flags id file Hfresh Hfind.
  unfold create_commit, open_file_description, state_find_file in *.
  simpl.
  destruct (Nat.eqb (vs_next_file_id state) id) eqn:Heq.
  - apply Nat.eqb_eq in Heq.
    pose proof (find_file_some_id (vs_files state) id file Hfind) as Hid.
    pose proof (find_file_some_in (vs_files state) id file Hfind) as Hin.
    exfalso.
    apply (Hfresh file Hin).
    rewrite Hid.
    symmetry.
    exact Heq.
  - exact Hfind.
Qed.

Lemma target_exists_after_create_commit_old :
  forall state parent name backend_object rights flags target,
    fresh_vnode_id state (vs_next_vnode_id state) ->
    fresh_file_id state (vs_next_file_id state) ->
    target_exists state target ->
    target_exists
      (vr_state
        (create_commit state parent name backend_object rights flags))
      target.
Proof.
  intros state parent name backend_object rights flags target Hvfresh Hffresh Htarget.
  destruct target as [id | id | id].
  - destruct Htarget as [node Hnode].
    exists node.
    apply state_find_vnode_after_create_commit_old; assumption.
  - destruct Htarget as [file Hfile].
    exists file.
    apply state_find_file_after_create_commit_old; assumption.
  - exact Htarget.
Qed.

Theorem create_commit_preserves_well_formed_state :
  forall state parent name backend_object rights flags,
    well_formed_state state ->
    In parent (vs_vnodes state) ->
    fresh_vnode_id state (vs_next_vnode_id state) ->
    fresh_file_id state (vs_next_file_id state) ->
    fresh_handle_id state (vs_next_handle_id state) ->
    fresh_backend_key state (vn_mount_id parent, backend_object) ->
    no_linked_child_named state parent name ->
    well_formed_state
      (vr_state
        (create_commit state parent name backend_object rights flags)).
Proof.
  intros state parent name backend_object rights flags
    Hwf Hparent_in Hvfresh Hffresh Hhfresh Hbfresh Hnochild.
  unfold well_formed_state in Hwf.
  destruct Hwf as
    [Hmount_ids
    [Hvnode_ids
    [Hfile_ids
    [Hhandle_ids
    [Hbackend_ids
    [Hlinked
    [Hmount_roots
    [Hvnode_mounts
    [Hfile_nodes
    [Hhandle_targets Hhandle_rights]]]]]]]]]].
  pose proof
    (create_commit_preserves_linked_child_uniqueness
      state parent name backend_object rights flags Hlinked Hnochild)
    as Hlinked_result.
  unfold well_formed_state.
  unfold create_commit, open_file_description.
  simpl.
  repeat split.
  - exact Hmount_ids.
  - constructor.
    + intro Hin.
      apply in_map_iff in Hin as [node [Hid Hin]].
      apply (Hvfresh node Hin).
      exact Hid.
    + exact Hvnode_ids.
  - constructor.
    + intro Hin.
      apply in_map_iff in Hin as [file [Hid Hin]].
      apply (Hffresh file Hin).
      exact Hid.
    + exact Hfile_ids.
  - constructor.
    + intro Hin.
      apply in_map_iff in Hin as [handle [Hid Hin]].
      apply (Hhfresh handle Hin).
      exact Hid.
    + exact Hhandle_ids.
  - constructor.
    + intro Hin.
      apply in_map_iff in Hin as [node [Hkey Hin]].
      apply (Hbfresh node Hin).
      exact Hkey.
    + exact Hbackend_ids.
  - exact Hlinked_result.
  - apply Forall_forall.
    intros mount_entry Hmount_in.
    rewrite Forall_forall in Hmount_roots.
    specialize (Hmount_roots mount_entry Hmount_in) as [root Hroot].
    exists root.
    unfold state_find_vnode in *.
    simpl.
    destruct (Nat.eqb (vs_next_vnode_id state) (m_root_vnode mount_entry)) eqn:Heq.
    + apply Nat.eqb_eq in Heq.
      pose proof
        (find_vnode_some_id
          (vs_vnodes state)
          (m_root_vnode mount_entry)
          root
          Hroot) as Hid.
      pose proof
        (find_vnode_some_in
          (vs_vnodes state)
          (m_root_vnode mount_entry)
          root
          Hroot) as Hin.
      exfalso.
      apply (Hvfresh root Hin).
      rewrite Hid.
      symmetry.
      exact Heq.
    + exact Hroot.
  - constructor.
    + rewrite Forall_forall in Hvnode_mounts.
      specialize (Hvnode_mounts parent Hparent_in) as [mount_entry Hmount].
      exists mount_entry.
      exact Hmount.
    + apply Forall_forall.
      intros node Hnode_in.
      rewrite Forall_forall in Hvnode_mounts.
      apply Hvnode_mounts.
      exact Hnode_in.
  - constructor.
    + split.
      * simpl.
        lia.
      * exists
          (mk_regular_vnode_with_backend
            (vs_next_vnode_id state)
            parent
            name
            backend_object).
        unfold state_find_vnode.
        simpl.
        rewrite Nat.eqb_refl.
        reflexivity.
    + apply Forall_forall.
      intros file Hfile_in.
      rewrite Forall_forall in Hfile_nodes.
      specialize (Hfile_nodes file Hfile_in) as [Hoffset [node Hnode]].
      split.
      * exact Hoffset.
      * exists node.
        unfold state_find_vnode in *.
        simpl.
        destruct (Nat.eqb (vs_next_vnode_id state) (vf_vnode_id file)) eqn:Heq.
        -- apply Nat.eqb_eq in Heq.
           pose proof
             (find_vnode_some_id
               (vs_vnodes state)
               (vf_vnode_id file)
               node
               Hnode) as Hid.
           pose proof
             (find_vnode_some_in
               (vs_vnodes state)
               (vf_vnode_id file)
               node
               Hnode) as Hin.
           exfalso.
           apply (Hvfresh node Hin).
           rewrite Hid.
           symmetry.
           exact Heq.
        -- exact Hnode.
  - constructor.
    + exists
        {|
          vf_id := vs_next_file_id state;
          vf_vnode_id := vs_next_vnode_id state;
          vf_offset := 0;
          vf_status_flags := file_status_flags_from_open flags;
          vf_rights := rights;
          vf_refcount := 1%nat;
        |}.
      unfold state_find_file.
      simpl.
      rewrite Nat.eqb_refl.
      reflexivity.
    + apply Forall_forall.
      intros handle Hhandle_in.
      rewrite Forall_forall in Hhandle_targets.
      specialize (Hhandle_targets handle Hhandle_in) as Htarget.
      apply target_exists_after_create_commit_old; assumption.
  - constructor.
    + exists
        {|
          vf_id := vs_next_file_id state;
          vf_vnode_id := vs_next_vnode_id state;
          vf_offset := 0;
          vf_status_flags := file_status_flags_from_open flags;
          vf_rights := rights;
          vf_refcount := 1%nat;
        |}.
      split.
      * unfold state_find_file.
        simpl.
        rewrite Nat.eqb_refl.
        reflexivity.
      * unfold rights_subset.
        intros right Hright.
        exact Hright.
    + apply Forall_forall.
      intros handle Hhandle_in.
      rewrite Forall_forall in Hhandle_rights.
      specialize (Hhandle_rights handle Hhandle_in) as Hrights.
      unfold handle_rights_subset_file_rights in *.
      destruct (vh_target handle) as [vnode_id | file_id | mount_id].
      * exact Hrights.
      * destruct Hrights as [file [Hfile Hsubset]].
        exists file.
        split.
        -- apply state_find_file_after_create_commit_old; assumption.
        -- exact Hsubset.
      * exact Hrights.
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

Theorem open_truncate_requires_write_right :
  forall node rights flags,
    has_open_flag VfsOpenNoFollow flags = false ->
    has_open_flag VfsOpenDirectory flags = false ->
    has_open_flag VfsOpenTruncate flags = true ->
    has_right VfsRightWrite rights = false ->
    open_existing_status node rights flags = FiledErrDenied.
Proof.
  intros node rights flags Hnofollow Hdir Htrunc Hwrite.
  apply open_existing_status_truncate_requires_write; assumption.
Qed.

Theorem open_truncate_requires_regular_file :
  forall node rights flags,
    has_open_flag VfsOpenNoFollow flags = false ->
    has_open_flag VfsOpenDirectory flags = false ->
    has_open_flag VfsOpenTruncate flags = true ->
    has_right VfsRightWrite rights = true ->
    vnode_is_regular node = false ->
    open_existing_status node rights flags = FiledErrInvalid.
Proof.
  intros node rights flags Hnofollow Hdir Htrunc Hwrite Hregular.
  unfold open_existing_status.
  rewrite Hnofollow.
  rewrite Hdir.
  rewrite Htrunc.
  rewrite Hwrite.
  rewrite Hregular.
  reflexivity.
Qed.

Theorem open_truncate_rejects_directory :
  forall node rights flags,
    vn_kind node = VnodeDirectory ->
    has_open_flag VfsOpenNoFollow flags = false ->
    has_open_flag VfsOpenDirectory flags = false ->
    has_open_flag VfsOpenTruncate flags = true ->
    has_right VfsRightWrite rights = true ->
    open_existing_status node rights flags = FiledErrInvalid.
Proof.
  intros node rights flags Hkind Hnofollow Hdir Htrunc Hwrite.
  apply open_truncate_requires_regular_file; try assumption.
  unfold vnode_is_regular.
  rewrite Hkind.
  reflexivity.
Qed.

Theorem open_truncate_returns_backend_truncate_decision :
  forall state node rights flags,
    has_open_flag VfsOpenTruncate flags = true ->
    vr_decision (open_file_description state node rights flags) =
      truncate_backend_decision node 0.
Proof.
  intros state node rights flags Htrunc.
  unfold open_file_description, open_truncate_decision, truncate_backend_decision.
  rewrite Htrunc.
  reflexivity.
Qed.

Theorem open_truncate_preserves_open_handle_semantics :
  forall state node rights flags handle_id file,
    has_open_flag VfsOpenTruncate flags = true ->
    vr_value (open_file_description state node rights flags) = Some handle_id ->
    state_find_file
      (vr_state (open_file_description state node rights flags))
      (vs_next_file_id state) = Some file ->
    handle_id = vs_next_handle_id state /\
    vf_vnode_id file = vn_id node /\
    vf_offset file = 0.
Proof.
  intros state node rights flags handle_id file Htrunc Hvalue Hfile.
  unfold open_file_description in Hvalue.
  inversion Hvalue; subst; clear Hvalue.
  unfold open_file_description in Hfile.
  unfold state_find_file in Hfile.
  simpl in Hfile.
  rewrite Nat.eqb_refl in Hfile.
  inversion Hfile; subst; clear Hfile.
  repeat split; reflexivity.
Qed.

Theorem ftruncate_requires_write_right :
  forall state handle_id handle length,
    state_find_handle state handle_id = Some handle ->
    has_right VfsRightWrite (vh_rights handle) = false ->
    vr_status (ftruncate_prepare state handle_id length) = FiledErrDenied.
Proof.
  intros state handle_id handle length Hhandle Hwrite.
  unfold ftruncate_prepare.
  rewrite Hhandle.
  rewrite Hwrite.
  reflexivity.
Qed.

Theorem ftruncate_rejects_negative_length :
  forall state handle_id handle length,
    state_find_handle state handle_id = Some handle ->
    has_right VfsRightWrite (vh_rights handle) = true ->
    z_nonnegative length = false ->
    vr_status (ftruncate_prepare state handle_id length) = FiledErrInvalid.
Proof.
  intros state handle_id handle length Hhandle Hwrite Hlength.
  unfold ftruncate_prepare.
  rewrite Hhandle.
  rewrite Hwrite.
  rewrite Hlength.
  reflexivity.
Qed.

Theorem ftruncate_requires_regular_file :
  forall state handle_id handle file_id file node length,
    state_find_handle state handle_id = Some handle ->
    vh_target handle = HandleFile file_id ->
    state_find_file state file_id = Some file ->
    state_find_vnode state (vf_vnode_id file) = Some node ->
    has_right VfsRightWrite (vh_rights handle) = true ->
    z_nonnegative length = true ->
    vnode_is_regular node = false ->
    vnode_is_directory node = false ->
    vr_status (ftruncate_prepare state handle_id length) = FiledErrInvalid.
Proof.
  intros state handle_id handle file_id file node length
    Hhandle Htarget Hfile Hnode Hwrite Hlength Hregular Hdir.
  unfold ftruncate_prepare.
  rewrite Hhandle.
  rewrite Hwrite.
  rewrite Hlength.
  rewrite Htarget.
  rewrite Hfile.
  rewrite Hnode.
  rewrite Hregular.
  rewrite Hdir.
  reflexivity.
Qed.

Theorem ftruncate_rejects_directory :
  forall state handle_id handle file_id file node length,
    state_find_handle state handle_id = Some handle ->
    vh_target handle = HandleFile file_id ->
    state_find_file state file_id = Some file ->
    state_find_vnode state (vf_vnode_id file) = Some node ->
    has_right VfsRightWrite (vh_rights handle) = true ->
    z_nonnegative length = true ->
    vnode_is_directory node = true ->
    vr_status (ftruncate_prepare state handle_id length) = FiledErrIsDir.
Proof.
  intros state handle_id handle file_id file node length
    Hhandle Htarget Hfile Hnode Hwrite Hlength Hdir.
  unfold ftruncate_prepare.
  rewrite Hhandle.
  rewrite Hwrite.
  rewrite Hlength.
  rewrite Htarget.
  rewrite Hfile.
  rewrite Hnode.
  destruct node; simpl in *.
  destruct vn_kind; try discriminate; reflexivity.
Qed.

Theorem ftruncate_returns_backend_truncate_decision :
  forall state handle_id handle file_id file node length,
    state_find_handle state handle_id = Some handle ->
    vh_target handle = HandleFile file_id ->
    state_find_file state file_id = Some file ->
    state_find_vnode state (vf_vnode_id file) = Some node ->
    has_right VfsRightWrite (vh_rights handle) = true ->
    z_nonnegative length = true ->
    vnode_is_regular node = true ->
    vr_status (ftruncate_prepare state handle_id length) = FiledOk /\
    vr_decision (ftruncate_prepare state handle_id length) =
      truncate_backend_decision node length.
Proof.
  intros state handle_id handle file_id file node length
    Hhandle Htarget Hfile Hnode Hwrite Hlength Hregular.
  unfold ftruncate_prepare.
  rewrite Hhandle.
  rewrite Hwrite.
  rewrite Hlength.
  rewrite Htarget.
  rewrite Hfile.
  rewrite Hnode.
  rewrite Hregular.
  split; reflexivity.
Qed.

Theorem ftruncate_prepare_state_unchanged :
  forall state handle_id length,
    vr_state (ftruncate_prepare state handle_id length) = state.
Proof.
  intros state handle_id length.
  unfold ftruncate_prepare.
  destruct (state_find_handle state handle_id) as [handle|]; try reflexivity.
  destruct (has_right VfsRightWrite (vh_rights handle)); try reflexivity.
  destruct (z_nonnegative length); try reflexivity.
  destruct (vh_target handle) as [vnode_id | file_id | mount_id]; try reflexivity.
  destruct (state_find_file state file_id) as [file|]; try reflexivity.
  destruct (state_find_vnode state (vf_vnode_id file)) as [node|]; try reflexivity.
  destruct (vnode_is_regular node); try reflexivity.
  destruct (vnode_is_directory node); try reflexivity.
Qed.

Theorem ftruncate_does_not_change_file_offset :
  forall state handle_id length file_id file,
    state_find_file state file_id = Some file ->
    state_find_file
      (vr_state (ftruncate_prepare state handle_id length))
      file_id = Some file.
Proof.
  intros state handle_id length file_id file Hfile.
  rewrite ftruncate_prepare_state_unchanged.
  exact Hfile.
Qed.

Theorem ftruncate_prepare_preserves_well_formed_state :
  forall state handle_id length,
    well_formed_state state ->
    well_formed_state (vr_state (ftruncate_prepare state handle_id length)).
Proof.
  intros state handle_id length Hwf.
  rewrite ftruncate_prepare_state_unchanged.
  exact Hwf.
Qed.

Theorem truncate_commit_preserves_well_formed_state :
  forall state,
    well_formed_state state ->
    well_formed_state (vr_state (truncate_commit state)).
Proof.
  intros state Hwf.
  unfold truncate_commit.
  exact Hwf.
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

Theorem rename_prepare_requires_rename_right :
  forall state root source target rights,
    has_right VfsRightRename rights = false ->
    vr_status (rename_prepare state root source target rights) = FiledErrDenied.
Proof.
  intros state root source target rights Hright.
  unfold rename_prepare.
  rewrite Hright.
  reflexivity.
Qed.

Theorem rename_prepare_rejects_invalid_source_name :
  forall state root source target source_parent source_name target_parent target_name rights,
    has_right VfsRightRename rights = true ->
    split_parent_components source = Some (source_parent, source_name) ->
    split_parent_components target = Some (target_parent, target_name) ->
    valid_new_child_name source_name = false ->
    vr_status (rename_prepare state root source target rights) = FiledErrInvalid.
Proof.
  intros state root source target source_parent source_name target_parent target_name rights
    Hright Hsource Htarget Hname.
  unfold rename_prepare.
  rewrite Hright.
  rewrite Hsource.
  rewrite Htarget.
  rewrite Hname.
  reflexivity.
Qed.

Theorem rename_prepare_rejects_invalid_target_name :
  forall state root source target source_parent source_name target_parent target_name rights,
    has_right VfsRightRename rights = true ->
    split_parent_components source = Some (source_parent, source_name) ->
    split_parent_components target = Some (target_parent, target_name) ->
    valid_new_child_name source_name = true ->
    valid_new_child_name target_name = false ->
    vr_status (rename_prepare state root source target rights) = FiledErrInvalid.
Proof.
  intros state root source target source_parent source_name target_parent target_name rights
    Hright Hsource Htarget Hsource_name Htarget_name.
  unfold rename_prepare.
  rewrite Hright.
  rewrite Hsource.
  rewrite Htarget.
  rewrite Hsource_name.
  rewrite Htarget_name.
  reflexivity.
Qed.

Theorem rename_prepare_source_parent_must_be_directory :
  forall state root source target source_parent_components source_name target_parent_components target_name rights source_walked source_parent,
    has_right VfsRightRename rights = true ->
    split_parent_components source = Some (source_parent_components, source_name) ->
    split_parent_components target = Some (target_parent_components, target_name) ->
    valid_new_child_name source_name = true ->
    valid_new_child_name target_name = true ->
    path_walk_with state root source_parent_components true =
      {|
        vr_status := FiledOk;
        vr_state := source_walked;
        vr_value := Some source_parent;
        vr_decision := no_vfs_decision;
      |} ->
    vnode_is_directory source_parent = false ->
    vr_status (rename_prepare state root source target rights) = FiledErrNotDir.
Proof.
  intros state root source target source_parent_components source_name
    target_parent_components target_name rights source_walked source_parent
    Hright Hsource Htarget Hsource_name Htarget_name Hwalk Hdir.
  unfold rename_prepare.
  rewrite Hright.
  rewrite Hsource.
  rewrite Htarget.
  rewrite Hsource_name.
  rewrite Htarget_name.
  rewrite Hwalk.
  rewrite Hdir.
  reflexivity.
Qed.

Theorem rename_prepare_source_must_exist :
  forall state root source target source_parent_components source_name target_parent_components target_name rights source_walked source_parent,
    has_right VfsRightRename rights = true ->
    split_parent_components source = Some (source_parent_components, source_name) ->
    split_parent_components target = Some (target_parent_components, target_name) ->
    valid_new_child_name source_name = true ->
    valid_new_child_name target_name = true ->
    path_walk_with state root source_parent_components true =
      {|
        vr_status := FiledOk;
        vr_state := source_walked;
        vr_value := Some source_parent;
        vr_decision := no_vfs_decision;
      |} ->
    vnode_is_directory source_parent = true ->
    lookup_child source_walked source_parent source_name = None ->
    vr_status (rename_prepare state root source target rights) = FiledErrNotFound.
Proof.
  intros state root source target source_parent_components source_name
    target_parent_components target_name rights source_walked source_parent
    Hright Hsource Htarget Hsource_name Htarget_name Hwalk Hdir Hlookup.
  unfold rename_prepare.
  rewrite Hright.
  rewrite Hsource.
  rewrite Htarget.
  rewrite Hsource_name.
  rewrite Htarget_name.
  rewrite Hwalk.
  rewrite Hdir.
  rewrite Hlookup.
  reflexivity.
Qed.

Theorem rename_prepare_target_parent_must_be_directory :
  forall state root source target source_parent_components source_name target_parent_components target_name rights
    source_walked source_parent source_node target_walked target_parent,
    has_right VfsRightRename rights = true ->
    split_parent_components source = Some (source_parent_components, source_name) ->
    split_parent_components target = Some (target_parent_components, target_name) ->
    valid_new_child_name source_name = true ->
    valid_new_child_name target_name = true ->
    path_walk_with state root source_parent_components true =
      {|
        vr_status := FiledOk;
        vr_state := source_walked;
        vr_value := Some source_parent;
        vr_decision := no_vfs_decision;
      |} ->
    vnode_is_directory source_parent = true ->
    lookup_child source_walked source_parent source_name = Some source_node ->
    path_walk_with source_walked root target_parent_components true =
      {|
        vr_status := FiledOk;
        vr_state := target_walked;
        vr_value := Some target_parent;
        vr_decision := no_vfs_decision;
      |} ->
    vnode_is_directory target_parent = false ->
    vr_status (rename_prepare state root source target rights) = FiledErrNotDir.
Proof.
  intros state root source target source_parent_components source_name
    target_parent_components target_name rights source_walked source_parent
    source_node target_walked target_parent
    Hright Hsource Htarget Hsource_name Htarget_name Hsource_walk Hsource_dir
    Hsource_lookup Htarget_walk Htarget_dir.
  unfold rename_prepare.
  rewrite Hright.
  rewrite Hsource.
  rewrite Htarget.
  rewrite Hsource_name.
  rewrite Htarget_name.
  rewrite Hsource_walk.
  rewrite Hsource_dir.
  rewrite Hsource_lookup.
  rewrite Htarget_walk.
  rewrite Htarget_dir.
  reflexivity.
Qed.

Theorem rename_prepare_rejects_cross_mount :
  forall state root source target source_parent_components source_name target_parent_components target_name rights
    source_walked source_parent source_node target_walked target_parent,
    has_right VfsRightRename rights = true ->
    split_parent_components source = Some (source_parent_components, source_name) ->
    split_parent_components target = Some (target_parent_components, target_name) ->
    valid_new_child_name source_name = true ->
    valid_new_child_name target_name = true ->
    path_walk_with state root source_parent_components true =
      {|
        vr_status := FiledOk;
        vr_state := source_walked;
        vr_value := Some source_parent;
        vr_decision := no_vfs_decision;
      |} ->
    vnode_is_directory source_parent = true ->
    lookup_child source_walked source_parent source_name = Some source_node ->
    path_walk_with source_walked root target_parent_components true =
      {|
        vr_status := FiledOk;
        vr_state := target_walked;
        vr_value := Some target_parent;
        vr_decision := no_vfs_decision;
      |} ->
    vnode_is_directory target_parent = true ->
    Nat.eqb (vn_mount_id source_parent) (vn_mount_id target_parent) = false ->
    vr_status (rename_prepare state root source target rights) = FiledErrCrossMount.
Proof.
  intros state root source target source_parent_components source_name
    target_parent_components target_name rights source_walked source_parent
    source_node target_walked target_parent
    Hright Hsource Htarget Hsource_name Htarget_name Hsource_walk Hsource_dir
    Hsource_lookup Htarget_walk Htarget_dir Hcross.
  unfold rename_prepare.
  rewrite Hright.
  rewrite Hsource.
  rewrite Htarget.
  rewrite Hsource_name.
  rewrite Htarget_name.
  rewrite Hsource_walk.
  rewrite Hsource_dir.
  rewrite Hsource_lookup.
  rewrite Htarget_walk.
  rewrite Htarget_dir.
  rewrite Hcross.
  reflexivity.
Qed.

Theorem rename_prepare_rejects_nonempty_directory_overwrite :
  forall state root source target source_parent_components source_name target_parent_components target_name rights
    source_walked source_parent source_node target_walked target_parent target_node,
    has_right VfsRightRename rights = true ->
    split_parent_components source = Some (source_parent_components, source_name) ->
    split_parent_components target = Some (target_parent_components, target_name) ->
    valid_new_child_name source_name = true ->
    valid_new_child_name target_name = true ->
    path_walk_with state root source_parent_components true =
      {|
        vr_status := FiledOk;
        vr_state := source_walked;
        vr_value := Some source_parent;
        vr_decision := no_vfs_decision;
      |} ->
    vnode_is_directory source_parent = true ->
    lookup_child source_walked source_parent source_name = Some source_node ->
    path_walk_with source_walked root target_parent_components true =
      {|
        vr_status := FiledOk;
        vr_state := target_walked;
        vr_value := Some target_parent;
        vr_decision := no_vfs_decision;
      |} ->
    vnode_is_directory target_parent = true ->
    Nat.eqb (vn_mount_id source_parent) (vn_mount_id target_parent) = true ->
    lookup_child target_walked target_parent target_name = Some target_node ->
    Nat.eqb (vn_id source_node) (vn_id target_node) = false ->
    vnode_is_directory target_node = true ->
    directory_empty target_walked target_node = false ->
    vr_status (rename_prepare state root source target rights) = FiledErrNotEmpty.
Proof.
  intros state root source target source_parent_components source_name
    target_parent_components target_name rights source_walked source_parent
    source_node target_walked target_parent target_node
    Hright Hsource Htarget Hsource_name Htarget_name Hsource_walk Hsource_dir
    Hsource_lookup Htarget_walk Htarget_dir Hmount Htarget_lookup Hid Hdir Hempty.
  unfold rename_prepare.
  rewrite Hright.
  rewrite Hsource.
  rewrite Htarget.
  rewrite Hsource_name.
  rewrite Htarget_name.
  rewrite Hsource_walk.
  rewrite Hsource_dir.
  rewrite Hsource_lookup.
  rewrite Htarget_walk.
  rewrite Htarget_dir.
  rewrite Hmount.
  rewrite Htarget_lookup.
  rewrite Hid.
  rewrite Hdir.
  rewrite Hempty.
  reflexivity.
Qed.

Theorem rename_prepare_returns_backend_rename_decision :
  forall state root source target source_parent_components source_name target_parent_components target_name rights
    source_walked source_parent source_node target_walked target_parent,
    has_right VfsRightRename rights = true ->
    split_parent_components source = Some (source_parent_components, source_name) ->
    split_parent_components target = Some (target_parent_components, target_name) ->
    valid_new_child_name source_name = true ->
    valid_new_child_name target_name = true ->
    path_walk_with state root source_parent_components true =
      {|
        vr_status := FiledOk;
        vr_state := source_walked;
        vr_value := Some source_parent;
        vr_decision := no_vfs_decision;
      |} ->
    vnode_is_directory source_parent = true ->
    lookup_child source_walked source_parent source_name = Some source_node ->
    path_walk_with source_walked root target_parent_components true =
      {|
        vr_status := FiledOk;
        vr_state := target_walked;
        vr_value := Some target_parent;
        vr_decision := no_vfs_decision;
      |} ->
    vnode_is_directory target_parent = true ->
    Nat.eqb (vn_mount_id source_parent) (vn_mount_id target_parent) = true ->
    lookup_child target_walked target_parent target_name = None ->
    vr_status (rename_prepare state root source target rights) = FiledOk /\
    vr_decision (rename_prepare state root source target rights) =
      rename_backend_decision source_parent source_name target_parent target_name.
Proof.
  intros state root source target source_parent_components source_name
    target_parent_components target_name rights source_walked source_parent
    source_node target_walked target_parent
    Hright Hsource Htarget Hsource_name Htarget_name Hsource_walk Hsource_dir
    Hsource_lookup Htarget_walk Htarget_dir Hmount Htarget_lookup.
  unfold rename_prepare.
  rewrite Hright.
  rewrite Hsource.
  rewrite Htarget.
  rewrite Hsource_name.
  rewrite Htarget_name.
  rewrite Hsource_walk.
  rewrite Hsource_dir.
  rewrite Hsource_lookup.
  rewrite Htarget_walk.
  rewrite Htarget_dir.
  rewrite Hmount.
  rewrite Htarget_lookup.
  split; reflexivity.
Qed.

Theorem lookup_child_in_ignores_unlinked_matching_head :
  forall mount parent name node,
    vn_linked node = false ->
    lookup_child_in [node] mount parent name = None.
Proof.
  intros mount parent name node Hunlinked.
  simpl.
  unfold vnode_child_name_matches.
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

Theorem lookup_child_in_matching_head :
  forall nodes mount parent name node,
    vnode_child_name_matches mount parent name node = true ->
    lookup_child_in (node :: nodes) mount parent name = Some node.
Proof.
  intros nodes mount parent name node Hmatch.
  simpl.
  rewrite Hmatch.
  reflexivity.
Qed.

Theorem lookup_child_in_none_without_match :
  forall nodes mount parent name,
    (forall node,
      In node nodes ->
      vnode_child_name_matches mount parent name node = false) ->
    lookup_child_in nodes mount parent name = None.
Proof.
  induction nodes as [| head rest IH]; intros mount parent name Hnone.
  - reflexivity.
  - simpl.
    rewrite Hnone.
    + apply IH.
      intros node Hin.
      apply Hnone.
      right.
      exact Hin.
    + left.
      reflexivity.
Qed.

Theorem lookup_child_in_some_matches :
  forall nodes mount parent name node,
    lookup_child_in nodes mount parent name = Some node ->
    In node nodes /\
    vnode_child_name_matches mount parent name node = true.
Proof.
  induction nodes as [| head rest IH]; intros mount parent name node Hlookup.
  - discriminate Hlookup.
  - simpl in Hlookup.
    destruct (vnode_child_name_matches mount parent name head) eqn:Hhead.
    + inversion Hlookup; subst; clear Hlookup.
      split.
      * left.
        reflexivity.
      * exact Hhead.
    + apply IH in Hlookup as [Hin Hmatch].
      split.
      * right.
        exact Hin.
      * exact Hmatch.
Qed.

Theorem lookup_child_some_matches :
  forall state directory name node,
    lookup_child state directory name = Some node ->
    In node (vs_vnodes state) /\
    vnode_child_name_matches
      (vn_mount_id directory)
      (vn_id directory)
      name
      node = true.
Proof.
  intros state directory name node Hlookup.
  unfold lookup_child in Hlookup.
  apply lookup_child_in_some_matches in Hlookup.
  exact Hlookup.
Qed.

Theorem lookup_child_in_exists_if_match :
  forall nodes mount parent name node,
    In node nodes ->
    vnode_child_name_matches mount parent name node = true ->
    exists found,
      lookup_child_in nodes mount parent name = Some found.
Proof.
  induction nodes as [| head rest IH]; intros mount parent name node Hin Hmatch.
  - contradiction.
  - simpl.
    destruct Hin as [Hhead_eq | Hin_rest].
    + subst head.
      rewrite Hmatch.
      exists node.
      reflexivity.
    + destruct (vnode_child_name_matches mount parent name head).
      * exists head.
        reflexivity.
      * apply (IH mount parent name node); assumption.
Qed.

Theorem vnode_child_name_matches_props :
  forall mount parent name node,
    vnode_child_name_matches mount parent name node = true ->
    vn_linked node = true /\
    vn_mount_id node = mount /\
    vn_parent node = Some parent /\
    vn_name node = name.
Proof.
  intros mount parent name node Hmatch.
  unfold vnode_child_name_matches in Hmatch.
  destruct (vn_parent node) as [actual_parent|] eqn:Hparent.
  - simpl in Hmatch.
    apply andb_true_iff in Hmatch as [Hleft Hname].
    apply andb_true_iff in Hleft as [Hleft Hlinked].
    apply andb_true_iff in Hleft as [Hmount Hparent_eq].
    apply Nat.eqb_eq in Hmount.
    apply Nat.eqb_eq in Hparent_eq.
    apply String.eqb_eq in Hname.
    split.
    + exact Hlinked.
    + split.
      * exact Hmount.
      * split.
        -- subst actual_parent.
           reflexivity.
        -- exact Hname.
  - simpl in Hmatch.
    destruct (Nat.eqb (vn_mount_id node) mount);
    destruct (vn_linked node);
    destruct (string_eqb (vn_name node) name);
    discriminate.
Qed.

Theorem lookup_child_unique_finds_matching_id :
  forall state directory name node,
    linked_child_name_unique state ->
    In node (vs_vnodes state) ->
    vnode_child_name_matches
      (vn_mount_id directory)
      (vn_id directory)
      name
      node = true ->
    exists found,
      lookup_child state directory name = Some found /\
      vn_id found = vn_id node.
Proof.
  intros state directory name node Hunique Hin Hmatch.
  unfold lookup_child.
  pose proof
    (lookup_child_in_exists_if_match
      (vs_vnodes state)
      (vn_mount_id directory)
      (vn_id directory)
      name
      node
      Hin
      Hmatch) as [found Hfound].
  exists found.
  split.
  - exact Hfound.
  - apply lookup_child_in_some_matches in Hfound as [Hfound_in Hfound_match].
    apply Hunique; try exact Hfound_in; try exact Hin.
    pose proof
      (vnode_child_name_matches_props
        (vn_mount_id directory)
        (vn_id directory)
        name
        found
        Hfound_match) as
      [Hfound_linked [Hfound_mount [Hfound_parent Hfound_name]]].
    pose proof
      (vnode_child_name_matches_props
        (vn_mount_id directory)
        (vn_id directory)
        name
        node
        Hmatch) as
      [Hnode_linked [Hnode_mount [Hnode_parent Hnode_name]]].
    repeat split; try assumption.
    + rewrite Hfound_mount.
      rewrite Hnode_mount.
      reflexivity.
    + rewrite Hfound_parent.
      rewrite Hnode_parent.
      reflexivity.
    + rewrite Hfound_name.
      rewrite Hnode_name.
      reflexivity.
Qed.

Theorem lookup_child_none_without_linked_child :
  forall state directory name,
    (forall node,
      In node (vs_vnodes state) ->
      vn_linked node = true ->
      vn_mount_id node = vn_mount_id directory ->
      vn_parent node = Some (vn_id directory) ->
      vn_name node = name ->
      False) ->
    lookup_child state directory name = None.
Proof.
  intros state directory name Hnone.
  unfold lookup_child.
  apply lookup_child_in_none_without_match.
  intros node Hin.
  unfold vnode_child_name_matches.
  destruct (vn_parent node) as [actual_parent|] eqn:Hparent; simpl.
  - destruct (Nat.eqb (vn_mount_id node) (vn_mount_id directory)) eqn:Hmount;
    destruct (Nat.eqb actual_parent (vn_id directory)) eqn:Hparent_eq;
    destruct (vn_linked node) eqn:Hlinked;
    destruct (string_eqb (vn_name node) name) eqn:Hname;
    try reflexivity.
    apply Nat.eqb_eq in Hmount.
    apply Nat.eqb_eq in Hparent_eq.
    apply String.eqb_eq in Hname.
    exfalso.
    apply (Hnone node Hin Hlinked Hmount).
    + rewrite Hparent.
      rewrite Hparent_eq.
      reflexivity.
    + exact Hname.
  - destruct (Nat.eqb (vn_mount_id node) (vn_mount_id directory));
    destruct (vn_linked node);
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

Lemma find_vnode_mark_vnode_unlinked_other :
  forall nodes target_id lookup_id node,
    lookup_id <> target_id ->
    find_vnode nodes lookup_id = Some node ->
    find_vnode (map (mark_vnode_unlinked_entry target_id) nodes) lookup_id =
      Some node.
Proof.
  induction nodes as [| head rest IH]; intros target_id lookup_id node Hneq Hfind.
  - discriminate Hfind.
  - simpl in Hfind.
    destruct (Nat.eqb (vn_id head) lookup_id) eqn:Hhead.
    + inversion Hfind; subst; clear Hfind.
      simpl.
      unfold mark_vnode_unlinked_entry.
      destruct (Nat.eqb (vn_id node) target_id) eqn:Htarget.
      * apply Nat.eqb_eq in Hhead.
        apply Nat.eqb_eq in Htarget.
        exfalso.
        apply Hneq.
        rewrite <- Hhead.
        exact Htarget.
      * simpl.
        rewrite Hhead.
        reflexivity.
    + simpl.
      unfold mark_vnode_unlinked_entry.
      destruct (Nat.eqb (vn_id head) target_id); simpl; rewrite Hhead.
      * apply IH; assumption.
      * apply IH; assumption.
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

Lemma find_vnode_rename_vnode_other :
  forall nodes moved_id lookup_id node parent name,
    lookup_id <> moved_id ->
    find_vnode nodes lookup_id = Some node ->
    find_vnode (map (rename_vnode_entry moved_id parent name) nodes) lookup_id =
      Some node.
Proof.
  induction nodes as [| head rest IH]; intros moved_id lookup_id node parent name Hneq Hfind.
  - discriminate Hfind.
  - simpl in Hfind.
    destruct (Nat.eqb (vn_id head) lookup_id) eqn:Hhead.
    + inversion Hfind; subst; clear Hfind.
      simpl.
      unfold rename_vnode_entry.
      destruct (Nat.eqb (vn_id node) moved_id) eqn:Hmoved.
      * apply Nat.eqb_eq in Hhead.
        apply Nat.eqb_eq in Hmoved.
        exfalso.
        apply Hneq.
        rewrite <- Hhead.
        exact Hmoved.
      * simpl.
        rewrite Hhead.
        reflexivity.
    + simpl.
      unfold rename_vnode_entry.
      destruct (Nat.eqb (vn_id head) moved_id); simpl; rewrite Hhead.
      * apply IH; assumption.
      * apply IH; assumption.
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

Theorem rename_commit_existing_target_unlinks_target :
  forall state source target_parent target_name target,
    Nat.eqb (vn_mount_id source) (vn_mount_id target_parent) = true ->
    Nat.eqb (vn_id source) (vn_id target) = false ->
    vnode_is_directory target = false ->
    state_find_vnode state (vn_id target) = Some target ->
    state_find_vnode
      (vr_state (rename_commit state source target_parent target_name (Some target)))
      (vn_id target) =
      Some (vnode_with_linked target false).
Proof.
  intros state source target_parent target_name target Hmount Hid Hdir Htarget.
  unfold rename_commit.
  rewrite Hmount.
  rewrite Hid.
  rewrite Hdir.
  unfold state_find_vnode, rename_vnode, set_vnodes.
  simpl.
  apply find_vnode_rename_vnode_other.
  - intro Heq.
    apply Nat.eqb_neq in Hid.
    symmetry in Heq.
    contradiction.
  - unfold state_find_vnode, mark_vnode_unlinked, set_vnodes in *.
    simpl.
    apply find_vnode_mark_vnode_unlinked_same.
    exact Htarget.
Qed.

Theorem rename_commit_existing_target_moves_source :
  forall state source target_parent target_name target,
    Nat.eqb (vn_mount_id source) (vn_mount_id target_parent) = true ->
    Nat.eqb (vn_id source) (vn_id target) = false ->
    vnode_is_directory target = false ->
    state_find_vnode state (vn_id source) = Some source ->
    state_find_vnode
      (vr_state (rename_commit state source target_parent target_name (Some target)))
      (vn_id source) =
      Some (vnode_with_parent_name source target_parent target_name).
Proof.
  intros state source target_parent target_name target Hmount Hid Hdir Hsource.
  unfold rename_commit.
  rewrite Hmount.
  rewrite Hid.
  rewrite Hdir.
  apply rename_vnode_moves_source.
  unfold state_find_vnode, mark_vnode_unlinked, set_vnodes.
  simpl.
  apply find_vnode_mark_vnode_unlinked_other.
  - apply Nat.eqb_neq.
    exact Hid.
  - exact Hsource.
Qed.

Theorem rename_commit_does_not_delete_open_source_vnode :
  forall state source target_parent target_name file,
    Nat.eqb (vn_mount_id source) (vn_mount_id target_parent) = true ->
    vf_vnode_id file = vn_id source ->
    state_find_vnode state (vf_vnode_id file) = Some source ->
    exists node,
      state_find_vnode
        (vr_state (rename_commit state source target_parent target_name None))
        (vf_vnode_id file) = Some node.
Proof.
  intros state source target_parent target_name file Hmount Hfile Hsource.
  rewrite Hfile in Hsource.
  exists (vnode_with_parent_name source target_parent target_name).
  rewrite Hfile.
  apply rename_commit_missing_target_moves_source; assumption.
Qed.

Theorem rename_commit_does_not_delete_open_target_vnode :
  forall state source target_parent target_name target file,
    Nat.eqb (vn_mount_id source) (vn_mount_id target_parent) = true ->
    Nat.eqb (vn_id source) (vn_id target) = false ->
    vnode_is_directory target = false ->
    vf_vnode_id file = vn_id target ->
    state_find_vnode state (vf_vnode_id file) = Some target ->
    exists node,
      state_find_vnode
        (vr_state (rename_commit state source target_parent target_name (Some target)))
        (vf_vnode_id file) = Some node.
Proof.
  intros state source target_parent target_name target file Hmount Hid Hdir Hfile Htarget.
  rewrite Hfile in Htarget.
  exists (vnode_with_linked target false).
  rewrite Hfile.
  apply rename_commit_existing_target_unlinks_target; try assumption.
Qed.

Theorem rename_commit_lookup_old_name_fails :
  forall state source target_parent target_name target_existing old_parent old_name,
    no_linked_child_named
      (vr_state (rename_commit state source target_parent target_name target_existing))
      old_parent
      old_name ->
    lookup_child
      (vr_state (rename_commit state source target_parent target_name target_existing))
      old_parent
      old_name = None.
Proof.
  intros state source target_parent target_name target_existing old_parent old_name Hnone.
  apply lookup_child_none_without_linked_child.
  exact Hnone.
Qed.

Theorem rename_commit_lookup_new_name_finds_source :
  forall state source target_parent target_name target_existing moved_source,
    linked_child_name_unique
      (vr_state (rename_commit state source target_parent target_name target_existing)) ->
    state_find_vnode
      (vr_state (rename_commit state source target_parent target_name target_existing))
      (vn_id source) = Some moved_source ->
    vnode_child_name_matches
      (vn_mount_id target_parent)
      (vn_id target_parent)
      target_name
      moved_source = true ->
    exists found,
      lookup_child
        (vr_state (rename_commit state source target_parent target_name target_existing))
        target_parent
        target_name = Some found /\
      vn_id found = vn_id source.
Proof.
  intros state source target_parent target_name target_existing moved_source
    Hunique Hfind Hmatch.
  unfold state_find_vnode in Hfind.
  pose proof
    (find_vnode_some_in
      (vs_vnodes
        (vr_state (rename_commit state source target_parent target_name target_existing)))
      (vn_id source)
      moved_source
      Hfind) as Hin.
  pose proof
    (find_vnode_some_id
      (vs_vnodes
        (vr_state (rename_commit state source target_parent target_name target_existing)))
      (vn_id source)
      moved_source
      Hfind) as Hid.
  pose proof
    (lookup_child_unique_finds_matching_id
      (vr_state (rename_commit state source target_parent target_name target_existing))
      target_parent
      target_name
      moved_source
      Hunique
      Hin
      Hmatch) as [found [Hlookup Hfound_id]].
  exists found.
  split.
  - exact Hlookup.
  - rewrite Hfound_id.
    exact Hid.
Qed.

Lemma mark_vnode_unlinked_preserves_linked_child_uniqueness :
  forall state id,
    linked_child_name_unique state ->
    linked_child_name_unique (mark_vnode_unlinked state id).
Proof.
  intros state id Hunique.
  unfold linked_child_name_unique in *.
  intros lhs rhs Hlhs Hrhs Hsame.
  unfold mark_vnode_unlinked, set_vnodes in Hlhs, Hrhs.
  simpl in Hlhs, Hrhs.
  apply in_map_iff in Hlhs as [lhs0 [Hlhs_eq Hlhs0_in]].
  apply in_map_iff in Hrhs as [rhs0 [Hrhs_eq Hrhs0_in]].
  destruct Hsame as [Hlhs_linked [Hrhs_linked [Hmount [Hparent Hname]]]].
  unfold mark_vnode_unlinked_entry in Hlhs_eq, Hrhs_eq.
  destruct (Nat.eqb (vn_id lhs0) id) eqn:Hlhs_marked;
  destruct (Nat.eqb (vn_id rhs0) id) eqn:Hrhs_marked.
  - subst lhs rhs.
    simpl.
    apply Nat.eqb_eq in Hlhs_marked.
    apply Nat.eqb_eq in Hrhs_marked.
    rewrite Hlhs_marked.
    symmetry.
    exact Hrhs_marked.
  - subst lhs.
    simpl in Hlhs_linked.
    discriminate.
  - subst rhs.
    simpl in Hrhs_linked.
    discriminate.
  - subst lhs rhs.
    apply Hunique; try exact Hlhs0_in; try exact Hrhs0_in.
    repeat split; assumption.
Qed.

Lemma mark_vnode_unlinked_preserves_source_mount :
  forall state id source target_parent,
    (forall node,
      In node (vs_vnodes state) ->
      vn_id node = vn_id source ->
      vn_mount_id node = vn_mount_id target_parent) ->
    (forall node,
      In node (vs_vnodes (mark_vnode_unlinked state id)) ->
      vn_id node = vn_id source ->
      vn_mount_id node = vn_mount_id target_parent).
Proof.
  intros state id source target_parent Hsource_mount node Hin Hid.
  unfold mark_vnode_unlinked, set_vnodes in Hin.
  simpl in Hin.
  apply in_map_iff in Hin as [original [Hnode Horiginal_in]].
  unfold mark_vnode_unlinked_entry in Hnode.
  destruct (Nat.eqb (vn_id original) id) eqn:Hmarked.
  - subst node.
    simpl in Hid.
    simpl.
    apply Hsource_mount; assumption.
  - subst node.
    apply Hsource_mount; assumption.
Qed.

Lemma rename_vnode_preserves_linked_child_uniqueness :
  forall state source target_parent target_name,
    linked_child_name_unique state ->
    (forall node,
      In node (vs_vnodes state) ->
      vn_id node = vn_id source ->
      vn_mount_id node = vn_mount_id target_parent) ->
    no_linked_child_named state target_parent target_name ->
    linked_child_name_unique
      (rename_vnode state (vn_id source) target_parent target_name).
Proof.
  intros state source target_parent target_name Hunique Hsource_mount Hnochild.
  unfold linked_child_name_unique in *.
  intros lhs rhs Hlhs Hrhs Hsame.
  unfold rename_vnode, set_vnodes in Hlhs, Hrhs.
  simpl in Hlhs, Hrhs.
  apply in_map_iff in Hlhs as [lhs0 [Hlhs_eq Hlhs0_in]].
  apply in_map_iff in Hrhs as [rhs0 [Hrhs_eq Hrhs0_in]].
  destruct Hsame as [Hlhs_linked [Hrhs_linked [Hmount [Hparent Hname]]]].
  unfold rename_vnode_entry in Hlhs_eq, Hrhs_eq.
  destruct (Nat.eqb (vn_id lhs0) (vn_id source)) eqn:Hlhs_moved;
  destruct (Nat.eqb (vn_id rhs0) (vn_id source)) eqn:Hrhs_moved.
  - subst lhs rhs.
    apply Nat.eqb_eq in Hlhs_moved.
    apply Nat.eqb_eq in Hrhs_moved.
    simpl.
    rewrite Hlhs_moved.
    symmetry.
    exact Hrhs_moved.
  - subst lhs.
    exfalso.
    apply Nat.eqb_eq in Hlhs_moved.
    apply Nat.eqb_neq in Hrhs_moved.
    apply (Hnochild rhs0 Hrhs0_in).
    + rewrite Hrhs_eq.
      exact Hrhs_linked.
    + simpl in Hmount.
      rewrite Hrhs_eq.
      rewrite <- Hmount.
      apply Hsource_mount; try exact Hlhs0_in.
      exact Hlhs_moved.
    + rewrite Hrhs_eq.
      symmetry.
      exact Hparent.
    + rewrite Hrhs_eq.
      symmetry.
      exact Hname.
  - subst rhs.
    exfalso.
    apply Nat.eqb_neq in Hlhs_moved.
    apply Nat.eqb_eq in Hrhs_moved.
    apply (Hnochild lhs0 Hlhs0_in).
    + rewrite Hlhs_eq.
      exact Hlhs_linked.
    + simpl in Hmount.
      rewrite Hlhs_eq.
      rewrite Hmount.
      apply Hsource_mount; try exact Hrhs0_in.
      exact Hrhs_moved.
    + rewrite Hlhs_eq.
      exact Hparent.
    + rewrite Hlhs_eq.
      exact Hname.
  - subst lhs rhs.
    apply Hunique; try exact Hlhs0_in; try exact Hrhs0_in.
    repeat split.
    + exact Hlhs_linked.
    + exact Hrhs_linked.
    + exact Hmount.
    + exact Hparent.
    + exact Hname.
Qed.

Theorem rename_commit_preserves_linked_child_uniqueness :
  forall state source target_parent target_name target_existing,
    linked_child_name_unique state ->
    (forall node,
      In node (vs_vnodes state) ->
      vn_id node = vn_id source ->
      vn_mount_id node = vn_mount_id target_parent) ->
    (match target_existing with
     | None => no_linked_child_named state target_parent target_name
     | Some target =>
         no_linked_child_named
           (mark_vnode_unlinked state (vn_id target))
           target_parent
           target_name
     end) ->
    linked_child_name_unique
      (vr_state (rename_commit state source target_parent target_name target_existing)).
Proof.
  intros state source target_parent target_name target_existing
    Hunique Hsource_mount Hnochild.
  unfold rename_commit.
  destruct (Nat.eqb (vn_mount_id source) (vn_mount_id target_parent)) eqn:Hmount.
  - destruct target_existing as [target|].
    + destruct (Nat.eqb (vn_id source) (vn_id target)) eqn:Hsame.
      * exact Hunique.
      * destruct
          (andb
            (vnode_is_directory target)
            (negb (directory_empty state target))) eqn:Hnonempty.
        -- exact Hunique.
        -- apply rename_vnode_preserves_linked_child_uniqueness.
           ++ apply mark_vnode_unlinked_preserves_linked_child_uniqueness.
              exact Hunique.
           ++ apply mark_vnode_unlinked_preserves_source_mount.
              exact Hsource_mount.
           ++ exact Hnochild.
    + apply rename_vnode_preserves_linked_child_uniqueness; assumption.
  - exact Hunique.
Qed.

Lemma map_vn_id_mark_vnode_unlinked_entry :
  forall nodes id,
    map vn_id (map (mark_vnode_unlinked_entry id) nodes) =
    map vn_id nodes.
Proof.
  induction nodes as [| head rest IH]; intros id.
  - reflexivity.
  - simpl.
    unfold mark_vnode_unlinked_entry at 1.
    destruct (Nat.eqb (vn_id head) id); simpl; rewrite (IH id); reflexivity.
Qed.

Lemma map_vn_id_rename_vnode_entry :
  forall nodes id parent name,
    map vn_id (map (rename_vnode_entry id parent name) nodes) =
    map vn_id nodes.
Proof.
  induction nodes as [| head rest IH]; intros id parent name.
  - reflexivity.
  - simpl.
    unfold rename_vnode_entry at 1.
    destruct (Nat.eqb (vn_id head) id); simpl; rewrite (IH id parent name); reflexivity.
Qed.

Lemma map_backend_key_mark_vnode_unlinked_entry :
  forall nodes id,
    map vnode_backend_key (map (mark_vnode_unlinked_entry id) nodes) =
    map vnode_backend_key nodes.
Proof.
  induction nodes as [| head rest IH]; intros id.
  - reflexivity.
  - simpl.
    unfold mark_vnode_unlinked_entry at 1.
    destruct (Nat.eqb (vn_id head) id); simpl; rewrite (IH id); reflexivity.
Qed.

Lemma map_backend_key_rename_vnode_entry :
  forall nodes id parent name,
    map vnode_backend_key (map (rename_vnode_entry id parent name) nodes) =
    map vnode_backend_key nodes.
Proof.
  induction nodes as [| head rest IH]; intros id parent name.
  - reflexivity.
  - simpl.
    unfold rename_vnode_entry at 1.
    destruct (Nat.eqb (vn_id head) id); simpl; rewrite (IH id parent name); reflexivity.
Qed.

Lemma find_vnode_mark_vnode_unlinked_exists :
  forall nodes mark_id lookup_id node,
    find_vnode nodes lookup_id = Some node ->
    exists node',
      find_vnode (map (mark_vnode_unlinked_entry mark_id) nodes) lookup_id =
        Some node'.
Proof.
  intros nodes mark_id lookup_id node Hfind.
  destruct (Nat.eqb lookup_id mark_id) eqn:Heq.
  - apply Nat.eqb_eq in Heq.
    subst lookup_id.
    exists (vnode_with_linked node false).
    apply find_vnode_mark_vnode_unlinked_same.
    exact Hfind.
  - apply Nat.eqb_neq in Heq.
    exists node.
    apply find_vnode_mark_vnode_unlinked_other; assumption.
Qed.

Lemma find_vnode_rename_vnode_exists :
  forall nodes moved_id lookup_id node parent name,
    find_vnode nodes lookup_id = Some node ->
    exists node',
      find_vnode (map (rename_vnode_entry moved_id parent name) nodes) lookup_id =
        Some node'.
Proof.
  intros nodes moved_id lookup_id node parent name Hfind.
  destruct (Nat.eqb lookup_id moved_id) eqn:Heq.
  - apply Nat.eqb_eq in Heq.
    subst lookup_id.
    exists (vnode_with_parent_name node parent name).
    apply find_vnode_rename_vnode_same.
    exact Hfind.
  - apply Nat.eqb_neq in Heq.
    exists node.
    apply find_vnode_rename_vnode_other; assumption.
Qed.

Lemma state_find_file_set_vnodes :
  forall state vnodes id,
    state_find_file (set_vnodes state vnodes) id =
    state_find_file state id.
Proof.
  reflexivity.
Qed.

Lemma state_find_mount_set_vnodes :
  forall state vnodes id,
    state_find_mount (set_vnodes state vnodes) id =
    state_find_mount state id.
Proof.
  reflexivity.
Qed.

Lemma set_vnodes_preserves_well_formed_state :
  forall state vnodes,
    well_formed_state state ->
    NoDup (map vn_id vnodes) ->
    NoDup (map vnode_backend_key vnodes) ->
    linked_child_name_unique (set_vnodes state vnodes) ->
    (forall id node,
      state_find_vnode state id = Some node ->
      exists node', find_vnode vnodes id = Some node') ->
    (forall node,
      In node vnodes ->
      exists mount_entry,
        state_find_mount state (vn_mount_id node) = Some mount_entry) ->
    well_formed_state (set_vnodes state vnodes).
Proof.
  intros state vnodes Hwf Hvnode_ids Hbackend_ids Hlinked Hfind_preserved Hnode_mounts.
  unfold well_formed_state in Hwf.
  destruct Hwf as
    [Hmount_ids
    [_Hvnode_ids
    [Hfile_ids
    [Hhandle_ids
    [_Hbackend_ids
    [_Hlinked
    [Hmount_roots
    [_Hvnode_mounts
    [Hfile_nodes
    [Hhandle_targets Hhandle_rights]]]]]]]]]].
  unfold well_formed_state.
  unfold set_vnodes.
  simpl.
  repeat split.
  - exact Hmount_ids.
  - exact Hvnode_ids.
  - exact Hfile_ids.
  - exact Hhandle_ids.
  - exact Hbackend_ids.
  - exact Hlinked.
  - apply Forall_forall.
    intros mount_entry Hmount_in.
    rewrite Forall_forall in Hmount_roots.
    specialize (Hmount_roots mount_entry Hmount_in) as [root Hroot].
    unfold state_find_vnode in Hroot.
    destruct (Hfind_preserved (m_root_vnode mount_entry) root Hroot) as [root' Hroot'].
    exists root'.
    exact Hroot'.
  - apply Forall_forall.
    intros node Hnode_in.
    apply Hnode_mounts.
    exact Hnode_in.
  - apply Forall_forall.
    intros file Hfile_in.
    rewrite Forall_forall in Hfile_nodes.
    specialize (Hfile_nodes file Hfile_in) as [Hoffset [node Hnode]].
    split.
    + exact Hoffset.
    + unfold state_find_vnode in Hnode.
      destruct (Hfind_preserved (vf_vnode_id file) node Hnode) as [node' Hnode'].
      exists node'.
      exact Hnode'.
  - apply Forall_forall.
    intros handle Hhandle_in.
    rewrite Forall_forall in Hhandle_targets.
    specialize (Hhandle_targets handle Hhandle_in) as Htarget.
    destruct (vh_target handle) as [vnode_id | file_id | mount_id].
    + destruct Htarget as [node Hnode].
      unfold state_find_vnode in Hnode.
      destruct (Hfind_preserved vnode_id node Hnode) as [node' Hnode'].
      exists node'.
      exact Hnode'.
    + exact Htarget.
    + exact Htarget.
  - apply Forall_forall.
    intros handle Hhandle_in.
    rewrite Forall_forall in Hhandle_rights.
    specialize (Hhandle_rights handle Hhandle_in) as Hrights.
    unfold handle_rights_subset_file_rights in *.
    destruct (vh_target handle) as [vnode_id | file_id | mount_id].
    + exact Hrights.
    + exact Hrights.
    + exact Hrights.
Qed.

Theorem mark_vnode_unlinked_preserves_well_formed_state :
  forall state id,
    well_formed_state state ->
    well_formed_state (mark_vnode_unlinked state id).
Proof.
  intros state id Hwf.
  unfold mark_vnode_unlinked.
  apply set_vnodes_preserves_well_formed_state.
  - exact Hwf.
  - unfold well_formed_state in Hwf.
    destruct Hwf as [_ [Hvnode_ids _]].
    rewrite map_vn_id_mark_vnode_unlinked_entry.
    exact Hvnode_ids.
  - unfold well_formed_state in Hwf.
    destruct Hwf as [_ [_ [_ [_ [Hbackend_ids _]]]]].
    rewrite map_backend_key_mark_vnode_unlinked_entry.
    exact Hbackend_ids.
  - apply mark_vnode_unlinked_preserves_linked_child_uniqueness.
    unfold well_formed_state in Hwf.
    destruct Hwf as [_ [_ [_ [_ [_ [Hlinked _]]]]]].
    exact Hlinked.
  - intros lookup_id node Hfind.
    unfold state_find_vnode in Hfind.
    apply (find_vnode_mark_vnode_unlinked_exists
      (vs_vnodes state)
      id
      lookup_id
      node).
    exact Hfind.
  - intros node Hnode_in.
    unfold well_formed_state in Hwf.
    destruct Hwf as [_ [_ [_ [_ [_ [_ [_ [Hvnode_mounts _]]]]]]]].
    apply in_map_iff in Hnode_in as [original [Hnode Horiginal_in]].
    rewrite Forall_forall in Hvnode_mounts.
    specialize (Hvnode_mounts original Horiginal_in) as [mount_entry Hmount].
    exists mount_entry.
    unfold mark_vnode_unlinked_entry in Hnode.
    destruct (Nat.eqb (vn_id original) id);
      subst node;
      simpl;
      exact Hmount.
Qed.

Theorem rename_vnode_preserves_well_formed_state :
  forall state source target_parent target_name,
    well_formed_state state ->
    linked_child_name_unique
      (rename_vnode state (vn_id source) target_parent target_name) ->
    well_formed_state
      (rename_vnode state (vn_id source) target_parent target_name).
Proof.
  intros state source target_parent target_name Hwf Hlinked.
  unfold rename_vnode.
  apply set_vnodes_preserves_well_formed_state.
  - exact Hwf.
  - unfold well_formed_state in Hwf.
    destruct Hwf as [_ [Hvnode_ids _]].
    rewrite map_vn_id_rename_vnode_entry.
    exact Hvnode_ids.
  - unfold well_formed_state in Hwf.
    destruct Hwf as [_ [_ [_ [_ [Hbackend_ids _]]]]].
    rewrite map_backend_key_rename_vnode_entry.
    exact Hbackend_ids.
  - exact Hlinked.
  - intros lookup_id node Hfind.
    unfold state_find_vnode in Hfind.
    apply (find_vnode_rename_vnode_exists
      (vs_vnodes state)
      (vn_id source)
      lookup_id
      node
      target_parent
      target_name).
    exact Hfind.
  - intros node Hnode_in.
    unfold well_formed_state in Hwf.
    destruct Hwf as [_ [_ [_ [_ [_ [_ [_ [Hvnode_mounts _]]]]]]]].
    apply in_map_iff in Hnode_in as [original [Hnode Horiginal_in]].
    rewrite Forall_forall in Hvnode_mounts.
    specialize (Hvnode_mounts original Horiginal_in) as [mount_entry Hmount].
    exists mount_entry.
    unfold rename_vnode_entry in Hnode.
    destruct (Nat.eqb (vn_id original) (vn_id source));
      subst node;
      simpl;
      exact Hmount.
Qed.

Theorem rename_commit_preserves_well_formed_state :
  forall state source target_parent target_name target_existing,
    well_formed_state state ->
    (forall node,
      In node (vs_vnodes state) ->
      vn_id node = vn_id source ->
      vn_mount_id node = vn_mount_id target_parent) ->
    (match target_existing with
     | None => no_linked_child_named state target_parent target_name
     | Some target =>
         no_linked_child_named
           (mark_vnode_unlinked state (vn_id target))
           target_parent
           target_name
     end) ->
    well_formed_state
      (vr_state (rename_commit state source target_parent target_name target_existing)).
Proof.
  intros state source target_parent target_name target_existing
    Hwf Hsource_mount Hnochild.
  pose proof Hwf as Hwf_for_linked.
  unfold well_formed_state in Hwf_for_linked.
  destruct Hwf_for_linked as [_ [_ [_ [_ [_ [Hlinked_state _]]]]]].
  unfold rename_commit.
  destruct (Nat.eqb (vn_mount_id source) (vn_mount_id target_parent)) eqn:Hmount.
  - destruct target_existing as [target|].
    + destruct (Nat.eqb (vn_id source) (vn_id target)) eqn:Hsame.
      * exact Hwf.
      * destruct
          (andb
            (vnode_is_directory target)
            (negb (directory_empty state target))) eqn:Hnonempty.
        -- exact Hwf.
        -- apply rename_vnode_preserves_well_formed_state.
           ++ apply mark_vnode_unlinked_preserves_well_formed_state.
              exact Hwf.
           ++ apply rename_vnode_preserves_linked_child_uniqueness.
              ** apply mark_vnode_unlinked_preserves_linked_child_uniqueness.
                 exact Hlinked_state.
              ** apply mark_vnode_unlinked_preserves_source_mount.
                 exact Hsource_mount.
              ** exact Hnochild.
    + apply rename_vnode_preserves_well_formed_state.
      * exact Hwf.
      * apply rename_vnode_preserves_linked_child_uniqueness; assumption.
  - exact Hwf.
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
