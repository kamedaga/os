From Stdlib Require Import Lists.List Strings.String ZArith.ZArith.
From Pacha.Filed Require Import
  ExecModel
  FiledRuntimeSpec
  FiledTypes
  VfsModel.

Import ListNotations.
Open Scope Z_scope.

Inductive filed_op : Type :=
| FiledOpOpenAt
    (root : vnode_id)
    (components : list string)
    (rights : list vfs_right)
    (flags : list vfs_open_flag)
| FiledOpClose
    (handle : handle_id)
| FiledOpDupAttenuate
    (handle : handle_id)
    (rights : list vfs_right)
    (fd_flags : list vfs_fd_flag)
| FiledOpPreadPrepare
    (handle : handle_id)
    (offset length : Z)
| FiledOpReadPrepare
    (handle : handle_id)
    (length : Z)
| FiledOpGetdentsPrepare
    (handle : handle_id)
| FiledOpMkdirAt
    (root : vnode_id)
    (components : list string)
    (rights : list vfs_right)
| FiledOpUnlinkAt
    (root : vnode_id)
    (components : list string)
    (rights : list vfs_right)
| FiledOpRmdir
    (root : vnode_id)
    (components : list string)
    (rights : list vfs_right)
| FiledOpRenameAt
    (root : vnode_id)
    (source_components target_components : list string)
    (rights : list vfs_right)
| FiledOpExecPath
    (root : vnode_id)
    (request : exec_request)
    (image : elf_image).

Inductive filed_response : Type :=
| FiledRespHandle
    (status : filed_status)
    (handle : option handle_id)
| FiledRespUnit
    (status : filed_status)
| FiledRespVnode
    (status : filed_status)
    (node : option vnode_id)
| FiledRespDecision
    (status : filed_status)
    (decision : option vfs_decision)
| FiledRespExec
    (status : filed_status)
    (plan : option exec_plan)
    (node : option vnode_id).

Record filed_request : Type := {
  frq_core : core_id;
  frq_client : client_id;
  frq_op : filed_op;
}.

Record filed_log_entry : Type := {
  fle_sequence : request_sequence;
  fle_core : core_id;
  fle_client : client_id;
  fle_status : filed_status;
}.

Record filed_runtime_state : Type := {
  frt_vfs : vfs_state;
  frt_next_sequence : request_sequence;
  frt_log : list filed_log_entry;
}.

Definition empty_filed_runtime_state : filed_runtime_state :=
  {|
    frt_vfs := empty_vfs_state;
    frt_next_sequence := 0%nat;
    frt_log := [];
  |}.

Definition response_status
    (response : filed_response)
  : filed_status :=
  match response with
  | FiledRespHandle status _ => status
  | FiledRespUnit status => status
  | FiledRespVnode status _ => status
  | FiledRespDecision status _ => status
  | FiledRespExec status _ _ => status
  end.

Definition apply_filed_op
    (state : vfs_state)
    (op : filed_op)
  : vfs_state * filed_response :=
  match op with
  | FiledOpOpenAt root components rights flags =>
      let result := openat_with_flags state root components rights flags in
      (vr_state result, FiledRespHandle (vr_status result) (vr_value result))
  | FiledOpClose handle =>
      let result := close state handle in
      (vr_state result, FiledRespUnit (vr_status result))
  | FiledOpDupAttenuate handle rights fd_flags =>
      let result := dup_attenuate state handle rights fd_flags in
      (vr_state result, FiledRespHandle (vr_status result) (vr_value result))
  | FiledOpPreadPrepare handle offset length =>
      let result := pread_prepare state handle offset length in
      (vr_state result, FiledRespDecision (vr_status result) (vr_value result))
  | FiledOpReadPrepare handle length =>
      let result := read_prepare state handle length in
      (vr_state result, FiledRespDecision (vr_status result) (vr_value result))
  | FiledOpGetdentsPrepare handle =>
      let result := getdents_prepare state handle in
      (vr_state result, FiledRespDecision (vr_status result) (vr_value result))
  | FiledOpMkdirAt root components rights =>
      let result := mkdirat state root components rights in
      (vr_state result, FiledRespVnode (vr_status result) (vr_value result))
  | FiledOpUnlinkAt root components rights =>
      let result := unlinkat state root components rights in
      (vr_state result, FiledRespUnit (vr_status result))
  | FiledOpRmdir root components rights =>
      let result := rmdir state root components rights in
      (vr_state result, FiledRespUnit (vr_status result))
  | FiledOpRenameAt root source_components target_components rights =>
      let result := renameat state root source_components target_components rights in
      (vr_state result, FiledRespUnit (vr_status result))
  | FiledOpExecPath root request image =>
      let result := filed_exec_path_with_handles state root request image (vs_handles state) in
      (fepr_state result, FiledRespExec (fepr_status result) (fepr_plan result) (fepr_vnode result))
  end.

Definition run_filed_request
    (runtime : filed_runtime_state)
    (request : filed_request)
  : filed_runtime_state * filed_response :=
  let sequence := frt_next_sequence runtime in
  let '(next_vfs, response) := apply_filed_op (frt_vfs runtime) (frq_op request) in
  let entry :=
    {|
      fle_sequence := sequence;
      fle_core := frq_core request;
      fle_client := frq_client request;
      fle_status := response_status response;
    |}
  in
  ( {|
      frt_vfs := next_vfs;
      frt_next_sequence := S sequence;
      frt_log := entry :: frt_log runtime;
    |}
  , response
  ).

Fixpoint run_filed_requests
    (runtime : filed_runtime_state)
    (requests : list filed_request)
  : filed_runtime_state :=
  match requests with
  | [] => runtime
  | request :: rest =>
      let '(next_runtime, _) := run_filed_request runtime request in
      run_filed_requests next_runtime rest
  end.

Definition sequence_log_wf
    (next : request_sequence)
    (log : list filed_log_entry)
  : Prop :=
  NoDup (map fle_sequence log) /\
  Forall (fun entry => fle_sequence entry < next)%nat log.

Definition filed_runtime_wf
    (runtime : filed_runtime_state)
  : Prop :=
  well_formed_state (frt_vfs runtime) /\
  sequence_log_wf (frt_next_sequence runtime) (frt_log runtime).

Definition filed_op_preserves_wf
    (op : filed_op)
  : Prop :=
  forall state next_state response,
    well_formed_state state ->
    apply_filed_op state op = (next_state, response) ->
    well_formed_state next_state.
