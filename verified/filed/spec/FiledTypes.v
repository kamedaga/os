From Stdlib Require Import Bool.Bool Lists.List Strings.String ZArith.ZArith.

Import ListNotations.
Open Scope string_scope.
Open Scope Z_scope.

Definition file_id := nat.
Definition handle_id := nat.
Definition mount_id := nat.
Definition vnode_id := nat.
Definition backend_id := nat.
Definition backend_object_id := nat.
Definition generation := nat.
Definition fd_id := nat.
Definition core_id := nat.
Definition client_id := nat.
Definition request_sequence := nat.

Definition invalid_id : nat := 0%nat.
Definition root_parent : option vnode_id := None.

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

Inductive fs_kind : Type :=
| FsExt4
| FsBtrfs
| FsSynthetic.

Inductive vnode_kind : Type :=
| VnodeRegular
| VnodeDirectory
| VnodeSymlink
| VnodeDevice
| VnodeFifo.

Inductive vfs_right : Type :=
| VfsRightLookup
| VfsRightRead
| VfsRightWrite
| VfsRightExec
| VfsRightStat
| VfsRightGetdents
| VfsRightCreate
| VfsRightRemove
| VfsRightRename.

Inductive vfs_fd_flag : Type :=
| VfsFdCloseOnExec.

Inductive vfs_file_status_flag : Type :=
| VfsFileAppend
| VfsFileNonblock
| VfsFileSync.

Inductive vfs_open_flag : Type :=
| VfsOpenCreate
| VfsOpenExclusive
| VfsOpenTruncate
| VfsOpenDirectory
| VfsOpenNoFollow
| VfsOpenCloseOnExec
| VfsOpenAppend
| VfsOpenNonblock
| VfsOpenSync.

Definition vfs_right_eqb
    (lhs rhs : vfs_right)
  : bool :=
  match lhs, rhs with
  | VfsRightLookup, VfsRightLookup => true
  | VfsRightRead, VfsRightRead => true
  | VfsRightWrite, VfsRightWrite => true
  | VfsRightExec, VfsRightExec => true
  | VfsRightStat, VfsRightStat => true
  | VfsRightGetdents, VfsRightGetdents => true
  | VfsRightCreate, VfsRightCreate => true
  | VfsRightRemove, VfsRightRemove => true
  | VfsRightRename, VfsRightRename => true
  | _, _ => false
  end.

Definition has_right
    (right : vfs_right)
    (rights : list vfs_right)
  : bool :=
  existsb (vfs_right_eqb right) rights.

Definition rights_subset
    (requested available : list vfs_right)
  : Prop :=
  forall right, In right requested -> In right available.

Definition has_all_rights
    (requested available : list vfs_right)
  : bool :=
  forallb (fun right => has_right right available) requested.

Definition vfs_fd_flag_eqb
    (lhs rhs : vfs_fd_flag)
  : bool :=
  match lhs, rhs with
  | VfsFdCloseOnExec, VfsFdCloseOnExec => true
  end.

Definition has_fd_flag
    (flag : vfs_fd_flag)
    (flags : list vfs_fd_flag)
  : bool :=
  existsb (vfs_fd_flag_eqb flag) flags.

Definition vfs_file_status_flag_eqb
    (lhs rhs : vfs_file_status_flag)
  : bool :=
  match lhs, rhs with
  | VfsFileAppend, VfsFileAppend => true
  | VfsFileNonblock, VfsFileNonblock => true
  | VfsFileSync, VfsFileSync => true
  | _, _ => false
  end.

Definition has_file_status_flag
    (flag : vfs_file_status_flag)
    (flags : list vfs_file_status_flag)
  : bool :=
  existsb (vfs_file_status_flag_eqb flag) flags.

Definition vfs_open_flag_eqb
    (lhs rhs : vfs_open_flag)
  : bool :=
  match lhs, rhs with
  | VfsOpenCreate, VfsOpenCreate => true
  | VfsOpenExclusive, VfsOpenExclusive => true
  | VfsOpenTruncate, VfsOpenTruncate => true
  | VfsOpenDirectory, VfsOpenDirectory => true
  | VfsOpenNoFollow, VfsOpenNoFollow => true
  | VfsOpenCloseOnExec, VfsOpenCloseOnExec => true
  | VfsOpenAppend, VfsOpenAppend => true
  | VfsOpenNonblock, VfsOpenNonblock => true
  | VfsOpenSync, VfsOpenSync => true
  | _, _ => false
  end.

Definition has_open_flag
    (flag : vfs_open_flag)
    (flags : list vfs_open_flag)
  : bool :=
  existsb (vfs_open_flag_eqb flag) flags.

Definition fd_flags_from_open
    (flags : list vfs_open_flag)
  : list vfs_fd_flag :=
  if has_open_flag VfsOpenCloseOnExec flags then
    [VfsFdCloseOnExec]
  else [].

Definition file_status_flags_from_open
    (flags : list vfs_open_flag)
  : list vfs_file_status_flag :=
  (if has_open_flag VfsOpenAppend flags then [VfsFileAppend] else []) ++
  (if has_open_flag VfsOpenNonblock flags then [VfsFileNonblock] else []) ++
  (if has_open_flag VfsOpenSync flags then [VfsFileSync] else []).

Inductive handle_target : Type :=
| HandleVnode (id : vnode_id)
| HandleFile (id : file_id)
| HandleMount (id : mount_id).

Definition string_eqb
    (lhs rhs : string)
  : bool :=
  String.eqb lhs rhs.

Definition z_nonnegative
    (value : Z)
  : bool :=
  0 <=? value.

Definition z_positive
    (value : Z)
  : bool :=
  0 <? value.

Definition z_range
    (lo value hi : Z)
  : bool :=
  andb (lo <=? value) (value <=? hi).
