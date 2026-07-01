From Stdlib Require Import Bool.Bool Lists.List Strings.String ZArith.ZArith.
From Pacha.Filed Require Import FiledTypes TmpfsModel.

Import ListNotations.
Open Scope string_scope.
Open Scope Z_scope.

Theorem tmpfs_empty_state_has_root :
  tmpfs_state_find empty_tmpfs_state tmpfs_root_object = Some mk_tmpfs_root.
Proof.
  reflexivity.
Qed.

Theorem tmpfs_root_lookup_dot_returns_root :
  tr_value (tmpfs_lookup_child empty_tmpfs_state tmpfs_root_object ".") =
    Some tmpfs_root_object.
Proof.
  reflexivity.
Qed.

Theorem tmpfs_create_then_lookup :
  let created := tmpfs_create empty_tmpfs_state tmpfs_root_object "file" 420 in
  tr_status created = FiledOk /\
  tr_value
    (tmpfs_lookup_child
      (tr_state created)
      tmpfs_root_object
      "file") = Some 2%nat.
Proof.
  simpl.
  split; reflexivity.
Qed.

Theorem tmpfs_write_then_read :
  let created := tmpfs_create empty_tmpfs_state tmpfs_root_object "file" 420 in
  let written := tmpfs_pwrite (tr_state created) 2%nat 0 [1; 2; 3] in
  tr_status written = FiledOk /\
  tr_value (tmpfs_pread (tr_state written) 2%nat 0 3) = Some [1; 2; 3].
Proof.
  simpl.
  split; reflexivity.
Qed.

Theorem tmpfs_unlink_directory_rejected :
  let created := tmpfs_mkdir empty_tmpfs_state tmpfs_root_object "dir" 493 in
  tr_status (tmpfs_unlink (tr_state created) tmpfs_root_object "dir") =
    FiledErrIsDir.
Proof.
  reflexivity.
Qed.

Theorem tmpfs_rmdir_nonempty_rejected :
  let dir_created := tmpfs_mkdir empty_tmpfs_state tmpfs_root_object "dir" 493 in
  let file_created := tmpfs_create (tr_state dir_created) 2%nat "file" 420 in
  tr_status (tmpfs_rmdir (tr_state file_created) tmpfs_root_object "dir") =
    FiledErrNotEmpty.
Proof.
  reflexivity.
Qed.

Theorem tmpfs_unlink_then_release_removes_object :
  let created := tmpfs_create empty_tmpfs_state tmpfs_root_object "file" 420 in
  let unlinked := tmpfs_unlink (tr_state created) tmpfs_root_object "file" in
  let released := tmpfs_release_object (tr_state unlinked) 2%nat in
  tmpfs_state_find (tr_state released) 2%nat = None.
Proof.
  reflexivity.
Qed.
