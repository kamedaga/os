From Stdlib Require Import Bool.Bool Lists.List Strings.String ZArith.ZArith.
Import ListNotations.
Open Scope string_scope.
Open Scope Z_scope.

Definition handle_id := nat.
Definition fd_id := nat.
Inductive filed_status : Type :=
| FiledOk
| FiledErrNotFound
| FiledErrNotDir
| FiledErrIsDir
| FiledErrExists
| FiledErrDenied
| FiledErrInvalid
| FiledErrCrossMount
| FiledErrNotEmpty
| FiledErrIo
| FiledErrUnsupported
| FiledErrBadFormat
| FiledErrInvalidImage
| FiledErrLoop
| FiledErrOverflow
| FiledErrFull.

(* Exec owns only launch capabilities. Path resolution, inode state and file
   offsets belong to Linux and are not modeled again by filed. *)
Record exec_handle : Type := {
  eh_id : handle_id;
  eh_close_on_exec : bool;
}.
Definition handle_has_cloexec (handle : exec_handle) : bool :=
  eh_close_on_exec handle.
Definition handle_inheritable (handle : exec_handle) : bool :=
  negb (handle_has_cloexec handle).
Definition z_nonnegative (value : Z) : bool := 0 <=? value.
Definition z_positive (value : Z) : bool := 0 <? value.
Definition z_range (lo value hi : Z) : bool :=
  andb (lo <=? value) (value <=? hi).
