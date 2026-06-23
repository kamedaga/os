From Stdlib Require Import Bool.Bool Lists.List ZArith.ZArith.
From Pacha.Filed Require Import FiledTypes VfsModel ExecModel.

Import ListNotations.
Open Scope Z_scope.

Record filed_exec_path_result : Type := {
  fepr_status : filed_status;
  fepr_state : vfs_state;
  fepr_plan : option exec_plan;
  fepr_vnode : option vnode_id;
}.

Definition filed_exec_path_fail
    (state : vfs_state)
    (status : filed_status)
  : filed_exec_path_result :=
  {|
    fepr_status := status;
    fepr_state := state;
    fepr_plan := None;
    fepr_vnode := None;
  |}.

Definition filed_exec_path_with_handles
    (state : vfs_state)
    (root : vnode_id)
    (request : exec_request)
    (image : elf_image)
    (handles : list vfs_handle)
  : filed_exec_path_result :=
  match path_walk state root (er_path request) with
  | {| vr_status := FiledOk; vr_state := walked_state; vr_value := Some node |} =>
      match vn_kind node with
      | VnodeDirectory =>
          filed_exec_path_fail walked_state FiledErrIsDir
      | _ =>
          match build_exec_plan_with_handles request image handles with
          | {| ex_status := FiledOk; ex_plan := Some plan |} =>
              {|
                fepr_status := FiledOk;
                fepr_state := walked_state;
                fepr_plan := Some plan;
                fepr_vnode := Some (vn_id node);
              |}
          | {| ex_status := status |} =>
              filed_exec_path_fail walked_state status
          end
      end
  | {| vr_status := status; vr_state := walked_state |} =>
      filed_exec_path_fail walked_state status
  end.

Definition filed_exec_path
    (state : vfs_state)
    (root : vnode_id)
    (request : exec_request)
    (image : elf_image)
  : filed_exec_path_result :=
  filed_exec_path_with_handles state root request image [].

Definition filed_exec_path_preserves_vfs_state
    (state : vfs_state)
    (root : vnode_id)
    (request : exec_request)
    (image : elf_image)
  : Prop :=
  let result := filed_exec_path state root request image in
  fepr_status result = FiledOk ->
  fepr_state result = state.

Theorem filed_exec_path_missing_root :
  forall state root request image,
    state_find_vnode state root = None ->
    fepr_status (filed_exec_path state root request image) = FiledErrNotFound.
Proof.
  intros state root request image Hmissing.
  unfold filed_exec_path, filed_exec_path_with_handles, path_walk, path_walk_with, path_walk_context.
  cbn [path_start_vnode_id pr_start pr_root pr_cwd].
  rewrite Hmissing.
  reflexivity.
Qed.
