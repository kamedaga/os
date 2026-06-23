From Stdlib Require Import Bool.Bool Lia Lists.List Strings.String ZArith.ZArith.
From Pacha.Filed Require Import FiledTypes.

Import ListNotations.
Open Scope string_scope.
Open Scope Z_scope.

Record mount : Type := {
  m_id : mount_id;
  m_root_vnode : vnode_id;
  m_backend : backend_id;
  m_fs_kind : fs_kind;
  m_flags : Z;
}.

Record vnode : Type := {
  vn_id : vnode_id;
  vn_mount_id : mount_id;
  vn_backend_object : backend_object_id;
  vn_kind : vnode_kind;
  vn_parent : option vnode_id;
  vn_name : string;
  vn_linked : bool;
  vn_symlink_target : option (list string);
  vn_generation : generation;
  vn_refcount : nat;
}.

Record vfile : Type := {
  vf_id : file_id;
  vf_vnode_id : vnode_id;
  vf_offset : Z;
  vf_status_flags : list vfs_file_status_flag;
  vf_rights : list vfs_right;
  vf_refcount : nat;
}.

Record vfs_handle : Type := {
  vh_id : handle_id;
  vh_target : handle_target;
  vh_rights : list vfs_right;
  vh_fd_flags : list vfs_fd_flag;
  vh_generation : generation;
}.

Record vfs_state : Type := {
  vs_mounts : list mount;
  vs_vnodes : list vnode;
  vs_files : list vfile;
  vs_handles : list vfs_handle;
  vs_next_mount_id : mount_id;
  vs_next_vnode_id : vnode_id;
  vs_next_file_id : file_id;
  vs_next_handle_id : handle_id;
}.

Inductive vfs_decision_kind : Type :=
| VfsDecisionNone
| VfsDecisionBackendCreate
| VfsDecisionBackendRead
| VfsDecisionBackendWrite
| VfsDecisionBackendGetdents
| VfsDecisionBackendTruncate
| VfsDecisionBackendRename.

Record vfs_decision : Type := {
  vd_kind : vfs_decision_kind;
  vd_vnode_id : vnode_id;
  vd_name : option string;
  vd_target_vnode_id : vnode_id;
  vd_target_name : option string;
  vd_offset : Z;
  vd_length : Z;
}.

Record vfs_result (A : Type) : Type := {
  vr_status : filed_status;
  vr_state : vfs_state;
  vr_value : option A;
  vr_decision : vfs_decision;
}.

Arguments vr_status {A} _.
Arguments vr_state {A} _.
Arguments vr_value {A} _.
Arguments vr_decision {A} _.

Definition no_vfs_decision : vfs_decision :=
  {|
    vd_kind := VfsDecisionNone;
    vd_vnode_id := invalid_id;
    vd_name := None;
    vd_target_vnode_id := invalid_id;
    vd_target_name := None;
    vd_offset := 0;
    vd_length := 0;
  |}.

Definition vfs_ok
    {A : Type}
    (state : vfs_state)
    (value : A)
  : vfs_result A :=
  {|
    vr_status := FiledOk;
    vr_state := state;
    vr_value := Some value;
    vr_decision := no_vfs_decision;
  |}.

Definition vfs_fail
    {A : Type}
    (state : vfs_state)
    (status : filed_status)
  : vfs_result A :=
  {|
    vr_status := status;
    vr_state := state;
    vr_value := None;
    vr_decision := no_vfs_decision;
  |}.

Definition empty_vfs_state : vfs_state :=
  {|
    vs_mounts := [];
    vs_vnodes := [];
    vs_files := [];
    vs_handles := [];
    vs_next_mount_id := 1%nat;
    vs_next_vnode_id := 1%nat;
    vs_next_file_id := 1%nat;
    vs_next_handle_id := 1%nat;
  |}.

Definition default_path_resolution_budget : nat := 256%nat.

Fixpoint find_mount
    (mounts : list mount)
    (id : mount_id)
  : option mount :=
  match mounts with
  | [] => None
  | entry :: rest =>
      if Nat.eqb (m_id entry) id then Some entry else find_mount rest id
  end.

Fixpoint find_vnode
    (vnodes : list vnode)
    (id : vnode_id)
  : option vnode :=
  match vnodes with
  | [] => None
  | entry :: rest =>
      if Nat.eqb (vn_id entry) id then Some entry else find_vnode rest id
  end.

Fixpoint find_file
    (files : list vfile)
    (id : file_id)
  : option vfile :=
  match files with
  | [] => None
  | entry :: rest =>
      if Nat.eqb (vf_id entry) id then Some entry else find_file rest id
  end.

Fixpoint find_handle
    (handles : list vfs_handle)
    (id : handle_id)
  : option vfs_handle :=
  match handles with
  | [] => None
  | entry :: rest =>
      if Nat.eqb (vh_id entry) id then Some entry else find_handle rest id
  end.

Definition state_find_mount
    (state : vfs_state)
    (id : mount_id)
  : option mount :=
  find_mount (vs_mounts state) id.

Definition state_find_vnode
    (state : vfs_state)
    (id : vnode_id)
  : option vnode :=
  find_vnode (vs_vnodes state) id.

Definition state_find_file
    (state : vfs_state)
    (id : file_id)
  : option vfile :=
  find_file (vs_files state) id.

Definition state_find_handle
    (state : vfs_state)
    (id : handle_id)
  : option vfs_handle :=
  find_handle (vs_handles state) id.

Definition vnode_is_directory
    (node : vnode)
  : bool :=
  match vn_kind node with
  | VnodeDirectory => true
  | _ => false
  end.

Definition vnode_is_regular
    (node : vnode)
  : bool :=
  match vn_kind node with
  | VnodeRegular => true
  | _ => false
  end.

Definition vnode_is_symlink
    (node : vnode)
  : bool :=
  match vn_kind node with
  | VnodeSymlink => true
  | _ => false
  end.

Definition vnode_backend_key
    (node : vnode)
  : mount_id * backend_object_id :=
  (vn_mount_id node, vn_backend_object node).

Definition same_linked_child_name
    (lhs rhs : vnode)
  : Prop :=
  vn_linked lhs = true /\
  vn_linked rhs = true /\
  vn_mount_id lhs = vn_mount_id rhs /\
  vn_parent lhs = vn_parent rhs /\
  vn_name lhs = vn_name rhs.

Definition linked_child_name_unique
    (state : vfs_state)
  : Prop :=
  forall lhs rhs,
    In lhs (vs_vnodes state) ->
    In rhs (vs_vnodes state) ->
    same_linked_child_name lhs rhs ->
    vn_id lhs = vn_id rhs.

Definition fresh_vnode_id
    (state : vfs_state)
    (id : vnode_id)
  : Prop :=
  forall node, In node (vs_vnodes state) -> vn_id node <> id.

Definition fresh_file_id
    (state : vfs_state)
    (id : file_id)
  : Prop :=
  forall file, In file (vs_files state) -> vf_id file <> id.

Definition fresh_handle_id
    (state : vfs_state)
    (id : handle_id)
  : Prop :=
  forall handle, In handle (vs_handles state) -> vh_id handle <> id.

Definition fresh_backend_key
    (state : vfs_state)
    (key : mount_id * backend_object_id)
  : Prop :=
  forall node, In node (vs_vnodes state) -> vnode_backend_key node <> key.

Definition no_linked_child_named
    (state : vfs_state)
    (parent : vnode)
    (name : string)
  : Prop :=
  forall node,
    In node (vs_vnodes state) ->
    vn_linked node = true ->
    vn_mount_id node = vn_mount_id parent ->
    vn_parent node = Some (vn_id parent) ->
    vn_name node = name ->
    False.

Definition target_exists
    (state : vfs_state)
    (target : handle_target)
  : Prop :=
  match target with
  | HandleVnode id => exists node, state_find_vnode state id = Some node
  | HandleFile id => exists file, state_find_file state id = Some file
  | HandleMount id => exists mount_entry, state_find_mount state id = Some mount_entry
  end.

Definition handle_rights_subset_file_rights
    (state : vfs_state)
    (handle : vfs_handle)
  : Prop :=
  match vh_target handle with
  | HandleFile id =>
      exists file,
        state_find_file state id = Some file /\
        rights_subset (vh_rights handle) (vf_rights file)
  | _ => True
  end.

Definition well_formed_state
    (state : vfs_state)
  : Prop :=
  NoDup (map m_id (vs_mounts state)) /\
  NoDup (map vn_id (vs_vnodes state)) /\
  NoDup (map vf_id (vs_files state)) /\
  NoDup (map vh_id (vs_handles state)) /\
  NoDup (map vnode_backend_key (vs_vnodes state)) /\
  linked_child_name_unique state /\
  Forall
    (fun mount_entry =>
      exists root, state_find_vnode state (m_root_vnode mount_entry) = Some root)
    (vs_mounts state) /\
  Forall
    (fun node =>
      exists mount_entry, state_find_mount state (vn_mount_id node) = Some mount_entry)
    (vs_vnodes state) /\
  Forall
    (fun file =>
      0 <= vf_offset file /\
      exists node, state_find_vnode state (vf_vnode_id file) = Some node)
    (vs_files state) /\
  Forall
    (fun handle => target_exists state (vh_target handle))
    (vs_handles state) /\
  Forall
    (handle_rights_subset_file_rights state)
    (vs_handles state).

Definition mk_root_vnode
    (id : vnode_id)
    (mount : mount_id)
    (backend_object : backend_object_id)
  : vnode :=
  {|
    vn_id := id;
    vn_mount_id := mount;
    vn_backend_object := backend_object;
    vn_kind := VnodeDirectory;
    vn_parent := None;
    vn_name := "/"%string;
    vn_linked := true;
    vn_symlink_target := None;
    vn_generation := 1%nat;
    vn_refcount := 1%nat;
  |}.

Definition mount_root
    (state : vfs_state)
    (kind : fs_kind)
    (backend : backend_id)
    (root_backend_object : backend_object_id)
  : vfs_result mount_id :=
  let mount_id := vs_next_mount_id state in
  let root_id := vs_next_vnode_id state in
  let root := mk_root_vnode root_id mount_id root_backend_object in
  let mount_entry :=
    {|
      m_id := mount_id;
      m_root_vnode := root_id;
      m_backend := backend;
      m_fs_kind := kind;
      m_flags := 0;
    |}
  in
  vfs_ok
    {|
      vs_mounts := mount_entry :: vs_mounts state;
      vs_vnodes := root :: vs_vnodes state;
      vs_files := vs_files state;
      vs_handles := vs_handles state;
      vs_next_mount_id := S mount_id;
      vs_next_vnode_id := S root_id;
      vs_next_file_id := vs_next_file_id state;
      vs_next_handle_id := vs_next_handle_id state;
    |}
    mount_id.

Definition vnode_child_name_matches
    (mount : mount_id)
    (parent : vnode_id)
    (name : string)
    (node : vnode)
  : bool :=
  let parent_matches :=
    match vn_parent node with
    | Some actual => Nat.eqb actual parent
    | None => false
    end
  in
  andb
    (andb
      (andb (Nat.eqb (vn_mount_id node) mount) parent_matches)
      (vn_linked node))
    (string_eqb (vn_name node) name).

Fixpoint lookup_child_in
    (nodes : list vnode)
    (mount : mount_id)
    (parent : vnode_id)
    (name : string)
  : option vnode :=
  match nodes with
  | [] => None
  | node :: rest =>
      if vnode_child_name_matches mount parent name node
      then Some node
      else lookup_child_in rest mount parent name
  end.

Definition lookup_child
    (state : vfs_state)
    (directory : vnode)
    (name : string)
  : option vnode :=
  lookup_child_in (vs_vnodes state) (vn_mount_id directory) (vn_id directory) name.

Fixpoint vnode_is_mount_root_in
    (mounts : list mount)
    (node : vnode)
  : bool :=
  match mounts with
  | [] => false
  | mount_entry :: rest =>
      if Nat.eqb (m_root_vnode mount_entry) (vn_id node) then
        true
      else vnode_is_mount_root_in rest node
  end.

Definition vnode_is_mount_root
    (state : vfs_state)
    (node : vnode)
  : bool :=
  vnode_is_mount_root_in (vs_mounts state) node.

Definition parent_or_self
    (state : vfs_state)
    (current : vnode)
  : option vnode :=
  if vnode_is_mount_root state current then
    Some current
  else
    match vn_parent current with
    | Some parent_id => state_find_vnode state parent_id
    | None => Some current
    end.

Fixpoint split_parent_components
    (components : list string)
  : option (list string * string) :=
  match components with
  | [] => None
  | name :: [] => Some ([], name)
  | head :: tail =>
      match split_parent_components tail with
      | Some (parent, name) => Some (head :: parent, name)
      | None => None
      end
  end.

Definition should_follow_symlink
    (rest : list string)
    (follow_final : bool)
  : bool :=
  match rest with
  | [] => follow_final
  | _ :: _ => true
  end.

Fixpoint path_walk_from_budget
    (budget : nat)
    (state : vfs_state)
    (current : vnode)
    (components : list string)
    (follow_final : bool)
  : vfs_result vnode :=
  match budget with
  | O =>
      match components with
      | [] => vfs_ok state current
      | _ :: _ => vfs_fail state FiledErrLoop
      end
  | S remaining_budget =>
      match components with
      | [] => vfs_ok state current
      | component :: rest =>
          if string_eqb component "." then
            path_walk_from_budget remaining_budget state current rest follow_final
          else if string_eqb component ".." then
            match parent_or_self state current with
            | Some parent =>
                path_walk_from_budget remaining_budget state parent rest follow_final
            | None => vfs_fail state FiledErrNotFound
            end
          else if vnode_is_directory current then
            match lookup_child state current component with
            | Some child =>
                if andb (vnode_is_symlink child)
                        (should_follow_symlink rest follow_final)
                then
                  match vn_symlink_target child with
                  | Some target =>
                      path_walk_from_budget
                        remaining_budget
                        state
                        current
                        (target ++ rest)
                        follow_final
                  | None => vfs_fail state FiledErrInvalid
                  end
                else path_walk_from_budget remaining_budget state child rest follow_final
            | None => vfs_fail state FiledErrNotFound
            end
          else vfs_fail state FiledErrNotDir
      end
  end.

Definition path_walk_from
    (state : vfs_state)
    (current : vnode)
    (components : list string)
  : vfs_result vnode :=
  path_walk_from_budget
    default_path_resolution_budget
    state
    current
    components
    true.

Inductive path_start : Type :=
| PathStartRoot
| PathStartCwd.

Record path_resolution_context : Type := {
  pr_root : vnode_id;
  pr_cwd : vnode_id;
  pr_start : path_start;
  pr_follow_final_symlink : bool;
  pr_budget : nat;
}.

Definition path_start_vnode_id
    (context : path_resolution_context)
  : vnode_id :=
  match pr_start context with
  | PathStartRoot => pr_root context
  | PathStartCwd => pr_cwd context
  end.

Definition path_walk_context
    (state : vfs_state)
    (context : path_resolution_context)
    (components : list string)
  : vfs_result vnode :=
  match state_find_vnode state (path_start_vnode_id context) with
  | Some start =>
      path_walk_from_budget
        (pr_budget context)
        state
        start
        components
        (pr_follow_final_symlink context)
  | None => vfs_fail state FiledErrNotFound
  end.

Definition path_walk_with
    (state : vfs_state)
    (root_id : vnode_id)
    (components : list string)
    (follow_final : bool)
  : vfs_result vnode :=
  path_walk_context
    state
    {|
      pr_root := root_id;
      pr_cwd := root_id;
      pr_start := PathStartRoot;
      pr_follow_final_symlink := follow_final;
      pr_budget := default_path_resolution_budget;
    |}
    components.

Definition path_walk_at
    (state : vfs_state)
    (root_id cwd_id : vnode_id)
    (start : path_start)
    (components : list string)
    (follow_final : bool)
  : vfs_result vnode :=
  path_walk_context
    state
    {|
      pr_root := root_id;
      pr_cwd := cwd_id;
      pr_start := start;
      pr_follow_final_symlink := follow_final;
      pr_budget := default_path_resolution_budget;
    |}
    components.

Definition path_walk
    (state : vfs_state)
    (root_id : vnode_id)
    (components : list string)
  : vfs_result vnode :=
  path_walk_with state root_id components true.

Definition open_truncate_decision
    (node : vnode)
    (flags : list vfs_open_flag)
  : vfs_decision :=
  if has_open_flag VfsOpenTruncate flags then
    {|
      vd_kind := VfsDecisionBackendTruncate;
      vd_vnode_id := vn_id node;
      vd_name := None;
      vd_target_vnode_id := invalid_id;
      vd_target_name := None;
      vd_offset := 0;
      vd_length := 0;
    |}
  else no_vfs_decision.

Definition open_existing_status
    (node : vnode)
    (rights : list vfs_right)
    (flags : list vfs_open_flag)
  : filed_status :=
  if andb (has_open_flag VfsOpenNoFollow flags)
          (match vn_kind node with VnodeSymlink => true | _ => false end)
  then FiledErrLoop
  else if andb (has_open_flag VfsOpenDirectory flags)
               (negb (vnode_is_directory node))
  then FiledErrNotDir
  else if has_open_flag VfsOpenTruncate flags then
    if negb (has_right VfsRightWrite rights) then FiledErrDenied
    else if vnode_is_regular node then FiledOk
    else FiledErrInvalid
  else FiledOk.

Definition open_file_description
    (state : vfs_state)
    (node : vnode)
    (rights : list vfs_right)
    (flags : list vfs_open_flag)
  : vfs_result handle_id :=
  let file_id := vs_next_file_id state in
  let handle_id := vs_next_handle_id state in
  let file :=
    {|
      vf_id := file_id;
      vf_vnode_id := vn_id node;
      vf_offset := 0;
      vf_status_flags := file_status_flags_from_open flags;
      vf_rights := rights;
      vf_refcount := 1%nat;
    |}
  in
  let handle :=
    {|
      vh_id := handle_id;
      vh_target := HandleFile file_id;
      vh_rights := rights;
      vh_fd_flags := fd_flags_from_open flags;
      vh_generation := 1%nat;
    |}
  in
  {|
    vr_status := FiledOk;
    vr_state :=
      {|
        vs_mounts := vs_mounts state;
        vs_vnodes := vs_vnodes state;
        vs_files := file :: vs_files state;
        vs_handles := handle :: vs_handles state;
        vs_next_mount_id := vs_next_mount_id state;
        vs_next_vnode_id := vs_next_vnode_id state;
        vs_next_file_id := S file_id;
        vs_next_handle_id := S handle_id;
      |};
    vr_value := Some handle_id;
    vr_decision := open_truncate_decision node flags;
  |}.

Definition open_existing
    (state : vfs_state)
    (node : vnode)
    (rights : list vfs_right)
    (flags : list vfs_open_flag)
  : vfs_result handle_id :=
  match open_existing_status node rights flags with
  | FiledOk => open_file_description state node rights flags
  | status => vfs_fail state status
  end.

Definition truncate_backend_decision
    (node : vnode)
    (length : Z)
  : vfs_decision :=
  {|
    vd_kind := VfsDecisionBackendTruncate;
    vd_vnode_id := vn_id node;
    vd_name := None;
    vd_target_vnode_id := invalid_id;
    vd_target_name := None;
    vd_offset := 0;
    vd_length := length;
  |}.

Definition truncate_prepare
    (state : vfs_state)
    (node : vnode)
    (rights : list vfs_right)
    (length : Z)
  : vfs_result vfs_decision :=
  if has_right VfsRightWrite rights then
    if z_nonnegative length then
      if vnode_is_regular node then
        let decision := truncate_backend_decision node length in
        {|
          vr_status := FiledOk;
          vr_state := state;
          vr_value := Some decision;
          vr_decision := decision;
        |}
      else if vnode_is_directory node then
        vfs_fail state FiledErrIsDir
      else vfs_fail state FiledErrInvalid
    else vfs_fail state FiledErrInvalid
  else vfs_fail state FiledErrDenied.

Definition ftruncate_prepare
    (state : vfs_state)
    (id : handle_id)
    (length : Z)
  : vfs_result vfs_decision :=
  match state_find_handle state id with
  | Some handle =>
      if has_right VfsRightWrite (vh_rights handle) then
        if z_nonnegative length then
          match vh_target handle with
          | HandleFile file_id =>
              match state_find_file state file_id with
              | Some file =>
                  match state_find_vnode state (vf_vnode_id file) with
                  | Some node =>
                      if vnode_is_regular node then
                        let decision := truncate_backend_decision node length in
                        {|
                          vr_status := FiledOk;
                          vr_state := state;
                          vr_value := Some decision;
                          vr_decision := decision;
                        |}
                      else if vnode_is_directory node then
                        vfs_fail state FiledErrIsDir
                      else vfs_fail state FiledErrInvalid
                  | None => vfs_fail state FiledErrInvalid
                  end
              | None => vfs_fail state FiledErrInvalid
              end
          | _ => vfs_fail state FiledErrInvalid
          end
        else vfs_fail state FiledErrInvalid
      else vfs_fail state FiledErrDenied
  | None => vfs_fail state FiledErrInvalid
  end.

Definition truncate_commit
    (state : vfs_state)
  : vfs_result unit :=
  vfs_ok state tt.

Definition mk_regular_vnode
    (id : vnode_id)
    (parent : vnode)
    (name : string)
  : vnode :=
  {|
    vn_id := id;
    vn_mount_id := vn_mount_id parent;
    vn_backend_object := id;
    vn_kind := VnodeRegular;
    vn_parent := Some (vn_id parent);
    vn_name := name;
    vn_linked := true;
    vn_symlink_target := None;
    vn_generation := 1%nat;
    vn_refcount := 1%nat;
  |}.

Definition mk_regular_vnode_with_backend
    (id : vnode_id)
    (parent : vnode)
    (name : string)
    (backend_object : backend_object_id)
  : vnode :=
  {|
    vn_id := id;
    vn_mount_id := vn_mount_id parent;
    vn_backend_object := backend_object;
    vn_kind := VnodeRegular;
    vn_parent := Some (vn_id parent);
    vn_name := name;
    vn_linked := true;
    vn_symlink_target := None;
    vn_generation := 1%nat;
    vn_refcount := 1%nat;
  |}.

Definition mk_directory_vnode
    (id : vnode_id)
    (parent : vnode)
    (name : string)
  : vnode :=
  {|
    vn_id := id;
    vn_mount_id := vn_mount_id parent;
    vn_backend_object := id;
    vn_kind := VnodeDirectory;
    vn_parent := Some (vn_id parent);
    vn_name := name;
    vn_linked := true;
    vn_symlink_target := None;
    vn_generation := 1%nat;
    vn_refcount := 1%nat;
  |}.

Definition vnode_with_linked
    (node : vnode)
    (linked : bool)
  : vnode :=
  {|
    vn_id := vn_id node;
    vn_mount_id := vn_mount_id node;
    vn_backend_object := vn_backend_object node;
    vn_kind := vn_kind node;
    vn_parent := vn_parent node;
    vn_name := vn_name node;
    vn_linked := linked;
    vn_symlink_target := vn_symlink_target node;
    vn_generation := vn_generation node;
    vn_refcount := vn_refcount node;
  |}.

Definition vnode_with_parent_name
    (node : vnode)
    (parent : vnode)
    (name : string)
  : vnode :=
  {|
    vn_id := vn_id node;
    vn_mount_id := vn_mount_id node;
    vn_backend_object := vn_backend_object node;
    vn_kind := vn_kind node;
    vn_parent := Some (vn_id parent);
    vn_name := name;
    vn_linked := true;
    vn_symlink_target := vn_symlink_target node;
    vn_generation := vn_generation node;
    vn_refcount := vn_refcount node;
  |}.

Definition update_vnode_entry
    (id : vnode_id)
    (replacement : vnode)
    (node : vnode)
  : vnode :=
  if Nat.eqb (vn_id node) id then replacement else node.

Definition mark_vnode_unlinked_entry
    (id : vnode_id)
    (node : vnode)
  : vnode :=
  if Nat.eqb (vn_id node) id then vnode_with_linked node false else node.

Definition rename_vnode_entry
    (id : vnode_id)
    (new_parent : vnode)
    (new_name : string)
    (node : vnode)
  : vnode :=
  if Nat.eqb (vn_id node) id then
    vnode_with_parent_name node new_parent new_name
  else node.

Definition set_vnodes
    (state : vfs_state)
    (vnodes : list vnode)
  : vfs_state :=
  {|
    vs_mounts := vs_mounts state;
    vs_vnodes := vnodes;
    vs_files := vs_files state;
    vs_handles := vs_handles state;
    vs_next_mount_id := vs_next_mount_id state;
    vs_next_vnode_id := vs_next_vnode_id state;
    vs_next_file_id := vs_next_file_id state;
    vs_next_handle_id := vs_next_handle_id state;
  |}.

Definition mark_vnode_unlinked
    (state : vfs_state)
    (id : vnode_id)
  : vfs_state :=
  set_vnodes state (map (mark_vnode_unlinked_entry id) (vs_vnodes state)).

Definition rename_vnode
    (state : vfs_state)
    (id : vnode_id)
    (new_parent : vnode)
    (new_name : string)
  : vfs_state :=
  set_vnodes state (map (rename_vnode_entry id new_parent new_name) (vs_vnodes state)).

Fixpoint has_linked_child_in
    (nodes : list vnode)
    (parent : vnode_id)
  : bool :=
  match nodes with
  | [] => false
  | node :: rest =>
      let parent_matches :=
        match vn_parent node with
        | Some actual => Nat.eqb actual parent
        | None => false
        end
      in
      if andb (vn_linked node) parent_matches then
        true
      else has_linked_child_in rest parent
  end.

Definition directory_empty
    (state : vfs_state)
    (directory : vnode)
  : bool :=
  negb (has_linked_child_in (vs_vnodes state) (vn_id directory)).

Definition valid_new_child_name
    (name : string)
  : bool :=
  negb (orb (string_eqb name ".") (string_eqb name "..")).

Definition create_backend_decision
    (parent : vnode)
    (name : string)
  : vfs_decision :=
  {|
    vd_kind := VfsDecisionBackendCreate;
    vd_vnode_id := vn_id parent;
    vd_name := Some name;
    vd_target_vnode_id := invalid_id;
    vd_target_name := None;
    vd_offset := 0;
    vd_length := 0;
  |}.

Definition create_prepare
    (state : vfs_state)
    (root_id : vnode_id)
    (components : list string)
    (rights : list vfs_right)
    (flags : list vfs_open_flag)
  : vfs_result vfs_decision :=
  if has_all_rights [VfsRightLookup] rights then
    if has_right VfsRightCreate rights then
      match split_parent_components components with
      | None => vfs_fail state FiledErrInvalid
      | Some (parent_components, name) =>
          if valid_new_child_name name then
            match path_walk_with state root_id parent_components true with
            | {| vr_status := FiledOk; vr_state := walked_state; vr_value := Some parent |} =>
                if vnode_is_directory parent then
                  match lookup_child walked_state parent name with
                  | Some _ => vfs_fail walked_state FiledErrExists
                  | None =>
                      let decision := create_backend_decision parent name in
                      {|
                        vr_status := FiledOk;
                        vr_state := walked_state;
                        vr_value := Some decision;
                        vr_decision := decision;
                      |}
                  end
                else vfs_fail walked_state FiledErrNotDir
            | {| vr_status := status; vr_state := walked_state |} =>
                vfs_fail walked_state status
            end
          else vfs_fail state FiledErrInvalid
      end
    else vfs_fail state FiledErrDenied
  else vfs_fail state FiledErrDenied.

Definition openat_create_prepare
    (state : vfs_state)
    (root_id : vnode_id)
    (components : list string)
    (rights : list vfs_right)
    (flags : list vfs_open_flag)
  : vfs_result vfs_decision :=
  if has_all_rights [VfsRightLookup] rights then
    match path_walk_with
            state
            root_id
            components
            (negb (has_open_flag VfsOpenNoFollow flags))
    with
    | {| vr_status := FiledOk; vr_state := walked_state; vr_value := Some _ |} =>
        if andb (has_open_flag VfsOpenCreate flags)
                (has_open_flag VfsOpenExclusive flags)
        then vfs_fail walked_state FiledErrExists
        else vfs_ok walked_state no_vfs_decision
    | {| vr_status := FiledErrNotFound; vr_state := walked_state |} =>
        if has_open_flag VfsOpenCreate flags then
          create_prepare walked_state root_id components rights flags
        else vfs_fail walked_state FiledErrNotFound
    | {| vr_status := status; vr_state := walked_state |} =>
        vfs_fail walked_state status
    end
  else vfs_fail state FiledErrDenied.

Definition create_commit
    (state : vfs_state)
    (parent : vnode)
    (name : string)
    (backend_object : backend_object_id)
    (rights : list vfs_right)
    (flags : list vfs_open_flag)
  : vfs_result handle_id :=
  let node_id := vs_next_vnode_id state in
  let node := mk_regular_vnode_with_backend node_id parent name backend_object in
  let created_state :=
    {|
      vs_mounts := vs_mounts state;
      vs_vnodes := node :: vs_vnodes state;
      vs_files := vs_files state;
      vs_handles := vs_handles state;
      vs_next_mount_id := vs_next_mount_id state;
      vs_next_vnode_id := S node_id;
      vs_next_file_id := vs_next_file_id state;
      vs_next_handle_id := vs_next_handle_id state;
    |}
  in
  open_file_description created_state node rights flags.

Definition create_regular_child
    (state : vfs_state)
    (parent : vnode)
    (name : string)
    (rights : list vfs_right)
    (flags : list vfs_open_flag)
  : vfs_result handle_id :=
  if has_right VfsRightCreate rights then
    if has_open_flag VfsOpenDirectory flags then
      vfs_fail state FiledErrInvalid
    else
      create_commit state parent name (vs_next_vnode_id state) rights flags
  else vfs_fail state FiledErrDenied.

Definition create_missing_path
    (state : vfs_state)
    (root_id : vnode_id)
    (components : list string)
    (rights : list vfs_right)
    (flags : list vfs_open_flag)
  : vfs_result handle_id :=
  match split_parent_components components with
  | None => vfs_fail state FiledErrInvalid
  | Some (parent_components, name) =>
      if orb (string_eqb name ".") (string_eqb name "..") then
        vfs_fail state FiledErrInvalid
      else
        match path_walk_with state root_id parent_components true with
        | {| vr_status := FiledOk; vr_state := walked_state; vr_value := Some parent |} =>
            if vnode_is_directory parent then
              match lookup_child walked_state parent name with
              | Some _ => vfs_fail walked_state FiledErrExists
              | None => create_regular_child walked_state parent name rights flags
              end
            else vfs_fail walked_state FiledErrNotDir
        | {| vr_status := status; vr_state := walked_state |} =>
            vfs_fail walked_state status
        end
  end.

Definition mkdirat
    (state : vfs_state)
    (root_id : vnode_id)
    (components : list string)
    (rights : list vfs_right)
  : vfs_result vnode_id :=
  if has_right VfsRightCreate rights then
    match split_parent_components components with
    | None => vfs_fail state FiledErrInvalid
    | Some (parent_components, name) =>
        if valid_new_child_name name then
          match path_walk_with state root_id parent_components true with
          | {| vr_status := FiledOk; vr_state := walked_state; vr_value := Some parent |} =>
              if vnode_is_directory parent then
                match lookup_child walked_state parent name with
                | Some _ => vfs_fail walked_state FiledErrExists
                | None =>
                    let node_id := vs_next_vnode_id walked_state in
                    let node := mk_directory_vnode node_id parent name in
                    vfs_ok
                      {|
                        vs_mounts := vs_mounts walked_state;
                        vs_vnodes := node :: vs_vnodes walked_state;
                        vs_files := vs_files walked_state;
                        vs_handles := vs_handles walked_state;
                        vs_next_mount_id := vs_next_mount_id walked_state;
                        vs_next_vnode_id := S node_id;
                        vs_next_file_id := vs_next_file_id walked_state;
                        vs_next_handle_id := vs_next_handle_id walked_state;
                      |}
                      node_id
                end
              else vfs_fail walked_state FiledErrNotDir
          | {| vr_status := status; vr_state := walked_state |} =>
              vfs_fail walked_state status
          end
        else vfs_fail state FiledErrInvalid
    end
  else vfs_fail state FiledErrDenied.

Definition unlinkat
    (state : vfs_state)
    (root_id : vnode_id)
    (components : list string)
    (rights : list vfs_right)
  : vfs_result unit :=
  if has_right VfsRightRemove rights then
    match split_parent_components components with
    | None => vfs_fail state FiledErrInvalid
    | Some (parent_components, name) =>
        if valid_new_child_name name then
          match path_walk_with state root_id parent_components true with
          | {| vr_status := FiledOk; vr_state := walked_state; vr_value := Some parent |} =>
              if vnode_is_directory parent then
                match lookup_child walked_state parent name with
                | Some child =>
                    if vnode_is_directory child then
                      vfs_fail walked_state FiledErrIsDir
                    else vfs_ok (mark_vnode_unlinked walked_state (vn_id child)) tt
                | None => vfs_fail walked_state FiledErrNotFound
                end
              else vfs_fail walked_state FiledErrNotDir
          | {| vr_status := status; vr_state := walked_state |} =>
              vfs_fail walked_state status
          end
        else vfs_fail state FiledErrInvalid
    end
  else vfs_fail state FiledErrDenied.

Definition rmdir
    (state : vfs_state)
    (root_id : vnode_id)
    (components : list string)
    (rights : list vfs_right)
  : vfs_result unit :=
  if has_right VfsRightRemove rights then
    match split_parent_components components with
    | None => vfs_fail state FiledErrInvalid
    | Some (parent_components, name) =>
        if valid_new_child_name name then
          match path_walk_with state root_id parent_components true with
          | {| vr_status := FiledOk; vr_state := walked_state; vr_value := Some parent |} =>
              if vnode_is_directory parent then
                match lookup_child walked_state parent name with
                | Some child =>
                    if vnode_is_directory child then
                      if directory_empty walked_state child then
                        vfs_ok (mark_vnode_unlinked walked_state (vn_id child)) tt
                      else vfs_fail walked_state FiledErrNotEmpty
                    else vfs_fail walked_state FiledErrNotDir
                | None => vfs_fail walked_state FiledErrNotFound
                end
              else vfs_fail walked_state FiledErrNotDir
          | {| vr_status := status; vr_state := walked_state |} =>
              vfs_fail walked_state status
          end
        else vfs_fail state FiledErrInvalid
    end
  else vfs_fail state FiledErrDenied.

Definition rename_backend_decision
    (source_parent : vnode)
    (source_name : string)
    (target_parent : vnode)
    (target_name : string)
  : vfs_decision :=
  {|
    vd_kind := VfsDecisionBackendRename;
    vd_vnode_id := vn_id source_parent;
    vd_name := Some source_name;
    vd_target_vnode_id := vn_id target_parent;
    vd_target_name := Some target_name;
    vd_offset := 0;
    vd_length := 0;
  |}.

Definition rename_prepare
    (state : vfs_state)
    (root_id : vnode_id)
    (source_components target_components : list string)
    (rights : list vfs_right)
  : vfs_result vfs_decision :=
  if has_right VfsRightRename rights then
    match split_parent_components source_components,
          split_parent_components target_components with
    | Some (source_parent_components, source_name),
      Some (target_parent_components, target_name) =>
        if andb (valid_new_child_name source_name) (valid_new_child_name target_name) then
          match path_walk_with state root_id source_parent_components true with
          | {| vr_status := FiledOk; vr_state := source_walked; vr_value := Some source_parent |} =>
              if vnode_is_directory source_parent then
                match lookup_child source_walked source_parent source_name with
                | Some source =>
                    match path_walk_with source_walked root_id target_parent_components true with
                    | {| vr_status := FiledOk; vr_state := target_walked; vr_value := Some target_parent |} =>
                        if vnode_is_directory target_parent then
                          if Nat.eqb (vn_mount_id source_parent) (vn_mount_id target_parent) then
                            match lookup_child target_walked target_parent target_name with
                            | Some target =>
                                if Nat.eqb (vn_id source) (vn_id target) then
                                  vfs_ok target_walked no_vfs_decision
                                else if andb (vnode_is_directory target)
                                             (negb (directory_empty target_walked target))
                                then vfs_fail target_walked FiledErrNotEmpty
                                else
                                  let decision :=
                                    rename_backend_decision
                                      source_parent
                                      source_name
                                      target_parent
                                      target_name
                                  in
                                  {|
                                    vr_status := FiledOk;
                                    vr_state := target_walked;
                                    vr_value := Some decision;
                                    vr_decision := decision;
                                  |}
                            | None =>
                                let decision :=
                                  rename_backend_decision
                                    source_parent
                                    source_name
                                    target_parent
                                    target_name
                                in
                                {|
                                  vr_status := FiledOk;
                                  vr_state := target_walked;
                                  vr_value := Some decision;
                                  vr_decision := decision;
                                |}
                            end
                          else vfs_fail target_walked FiledErrCrossMount
                        else vfs_fail target_walked FiledErrNotDir
                    | {| vr_status := status; vr_state := target_walked |} =>
                        vfs_fail target_walked status
                    end
                | None => vfs_fail source_walked FiledErrNotFound
                end
              else vfs_fail source_walked FiledErrNotDir
          | {| vr_status := status; vr_state := source_walked |} =>
              vfs_fail source_walked status
          end
        else vfs_fail state FiledErrInvalid
    | _, _ => vfs_fail state FiledErrInvalid
    end
  else vfs_fail state FiledErrDenied.

Definition rename_commit
    (state : vfs_state)
    (source : vnode)
    (target_parent : vnode)
    (target_name : string)
    (target_existing : option vnode)
  : vfs_result unit :=
  if Nat.eqb (vn_mount_id source) (vn_mount_id target_parent) then
    match target_existing with
    | Some target =>
        if Nat.eqb (vn_id source) (vn_id target) then
          vfs_ok state tt
        else if andb (vnode_is_directory target)
                     (negb (directory_empty state target))
        then vfs_fail state FiledErrNotEmpty
        else
          let without_target := mark_vnode_unlinked state (vn_id target) in
          vfs_ok
            (rename_vnode without_target (vn_id source) target_parent target_name)
            tt
    | None =>
        vfs_ok
          (rename_vnode state (vn_id source) target_parent target_name)
          tt
    end
  else vfs_fail state FiledErrCrossMount.

Definition renameat
    (state : vfs_state)
    (root_id : vnode_id)
    (source_components target_components : list string)
    (rights : list vfs_right)
  : vfs_result unit :=
  if has_right VfsRightRename rights then
    match split_parent_components source_components,
          split_parent_components target_components with
    | Some (source_parent_components, source_name),
      Some (target_parent_components, target_name) =>
        if andb (valid_new_child_name source_name) (valid_new_child_name target_name) then
          match path_walk_with state root_id source_parent_components true with
          | {| vr_status := FiledOk; vr_state := source_walked; vr_value := Some source_parent |} =>
              if vnode_is_directory source_parent then
                match lookup_child source_walked source_parent source_name with
                | Some source =>
                    match path_walk_with source_walked root_id target_parent_components true with
                    | {| vr_status := FiledOk; vr_state := target_walked; vr_value := Some target_parent |} =>
                        if vnode_is_directory target_parent then
                          if Nat.eqb (vn_mount_id source_parent) (vn_mount_id target_parent) then
                            rename_commit
                              target_walked
                              source
                              target_parent
                              target_name
                              (lookup_child target_walked target_parent target_name)
                          else vfs_fail target_walked FiledErrCrossMount
                        else vfs_fail target_walked FiledErrNotDir
                    | {| vr_status := status; vr_state := target_walked |} =>
                        vfs_fail target_walked status
                    end
                | None => vfs_fail source_walked FiledErrNotFound
                end
              else vfs_fail source_walked FiledErrNotDir
          | {| vr_status := status; vr_state := source_walked |} =>
              vfs_fail source_walked status
          end
        else vfs_fail state FiledErrInvalid
    | _, _ => vfs_fail state FiledErrInvalid
    end
  else vfs_fail state FiledErrDenied.

Definition openat_with_flags
    (state : vfs_state)
    (root_id : vnode_id)
    (components : list string)
    (rights : list vfs_right)
    (flags : list vfs_open_flag)
  : vfs_result handle_id :=
  if has_all_rights [VfsRightLookup] rights then
    match path_walk_with
            state
            root_id
            components
            (negb (has_open_flag VfsOpenNoFollow flags))
    with
    | {| vr_status := FiledOk; vr_state := walked_state; vr_value := Some node |} =>
        if andb (has_open_flag VfsOpenCreate flags)
                (has_open_flag VfsOpenExclusive flags)
        then vfs_fail walked_state FiledErrExists
        else open_existing walked_state node rights flags
    | {| vr_status := FiledErrNotFound; vr_state := walked_state |} =>
        if has_open_flag VfsOpenCreate flags then
          create_missing_path walked_state root_id components rights flags
        else vfs_fail walked_state FiledErrNotFound
    | {| vr_status := status; vr_state := walked_state |} =>
        vfs_fail walked_state status
    end
  else vfs_fail state FiledErrDenied.

Definition openat
    (state : vfs_state)
    (root_id : vnode_id)
    (components : list string)
    (rights : list vfs_right)
  : vfs_result handle_id :=
  openat_with_flags state root_id components rights [].

Definition close
    (state : vfs_state)
    (id : handle_id)
  : vfs_result unit :=
  match state_find_handle state id with
  | None => vfs_fail state FiledErrInvalid
  | Some _ =>
      vfs_ok
        {|
          vs_mounts := vs_mounts state;
          vs_vnodes := vs_vnodes state;
          vs_files := vs_files state;
          vs_handles := filter (fun handle => negb (Nat.eqb (vh_id handle) id)) (vs_handles state);
          vs_next_mount_id := vs_next_mount_id state;
          vs_next_vnode_id := vs_next_vnode_id state;
          vs_next_file_id := vs_next_file_id state;
          vs_next_handle_id := vs_next_handle_id state;
        |}
        tt
  end.

Definition bump_file_refcount_entry
    (id : file_id)
    (file : vfile)
  : vfile :=
  if Nat.eqb (vf_id file) id then
    {|
      vf_id := vf_id file;
      vf_vnode_id := vf_vnode_id file;
      vf_offset := vf_offset file;
      vf_status_flags := vf_status_flags file;
      vf_rights := vf_rights file;
      vf_refcount := S (vf_refcount file);
    |}
  else file.

Definition bump_file_refcount
    (state : vfs_state)
    (id : file_id)
  : vfs_state :=
  {|
    vs_mounts := vs_mounts state;
    vs_vnodes := vs_vnodes state;
    vs_files := map (bump_file_refcount_entry id) (vs_files state);
    vs_handles := vs_handles state;
    vs_next_mount_id := vs_next_mount_id state;
    vs_next_vnode_id := vs_next_vnode_id state;
    vs_next_file_id := vs_next_file_id state;
    vs_next_handle_id := vs_next_handle_id state;
  |}.

Definition dup_base_state
    (state : vfs_state)
    (target : handle_target)
  : option vfs_state :=
  match target with
  | HandleFile file_id =>
      match state_find_file state file_id with
      | Some _ => Some (bump_file_refcount state file_id)
      | None => None
      end
  | _ => Some state
  end.

Definition file_with_offset
    (file : vfile)
    (offset : Z)
  : vfile :=
  {|
    vf_id := vf_id file;
    vf_vnode_id := vf_vnode_id file;
    vf_offset := offset;
    vf_status_flags := vf_status_flags file;
    vf_rights := vf_rights file;
    vf_refcount := vf_refcount file;
  |}.

Definition set_file_offset_entry
    (id : file_id)
    (offset : Z)
    (file : vfile)
  : vfile :=
  if Nat.eqb (vf_id file) id then file_with_offset file offset else file.

Definition set_file_offset
    (state : vfs_state)
    (id : file_id)
    (offset : Z)
  : vfs_state :=
  {|
    vs_mounts := vs_mounts state;
    vs_vnodes := vs_vnodes state;
    vs_files := map (set_file_offset_entry id offset) (vs_files state);
    vs_handles := vs_handles state;
    vs_next_mount_id := vs_next_mount_id state;
    vs_next_vnode_id := vs_next_vnode_id state;
    vs_next_file_id := vs_next_file_id state;
    vs_next_handle_id := vs_next_handle_id state;
  |}.

Definition dup_attenuate
    (state : vfs_state)
    (id : handle_id)
    (new_rights : list vfs_right)
    (new_fd_flags : list vfs_fd_flag)
  : vfs_result handle_id :=
  match state_find_handle state id with
  | None => vfs_fail state FiledErrInvalid
  | Some handle =>
      if has_all_rights new_rights (vh_rights handle) then
        match dup_base_state state (vh_target handle) with
        | None => vfs_fail state FiledErrInvalid
        | Some base_state =>
        let new_id := vs_next_handle_id base_state in
        let new_handle :=
          {|
            vh_id := new_id;
            vh_target := vh_target handle;
            vh_rights := new_rights;
            vh_fd_flags := new_fd_flags;
            vh_generation := S (vh_generation handle);
          |}
        in
        vfs_ok
          {|
            vs_mounts := vs_mounts base_state;
            vs_vnodes := vs_vnodes base_state;
            vs_files := vs_files base_state;
            vs_handles := new_handle :: vs_handles base_state;
            vs_next_mount_id := vs_next_mount_id base_state;
            vs_next_vnode_id := vs_next_vnode_id base_state;
            vs_next_file_id := vs_next_file_id base_state;
            vs_next_handle_id := S new_id;
          |}
          new_id
        end
      else vfs_fail state FiledErrDenied
  end.

Definition dup_attenuate_default
    (state : vfs_state)
    (id : handle_id)
    (new_rights : list vfs_right)
  : vfs_result handle_id :=
  dup_attenuate state id new_rights [].

Definition handle_has_cloexec
    (handle : vfs_handle)
  : bool :=
  has_fd_flag VfsFdCloseOnExec (vh_fd_flags handle).

Definition handle_inheritable
    (handle : vfs_handle)
  : bool :=
  negb (handle_has_cloexec handle).

Definition inheritable_handles
    (state : vfs_state)
  : list vfs_handle :=
  filter handle_inheritable (vs_handles state).

Definition read_prepare
    (state : vfs_state)
    (id : handle_id)
    (length : Z)
  : vfs_result vfs_decision :=
  match state_find_handle state id with
  | Some handle =>
      if andb (has_right VfsRightRead (vh_rights handle))
              (z_nonnegative length)
      then
        match vh_target handle with
        | HandleFile file_id =>
            match state_find_file state file_id with
            | Some file =>
                match state_find_vnode state (vf_vnode_id file) with
                | Some node =>
                    if vnode_is_regular node then
                      let old_offset := vf_offset file in
                      let decision :=
                        {|
                          vd_kind := VfsDecisionBackendRead;
                          vd_vnode_id := vn_id node;
                          vd_name := None;
                          vd_target_vnode_id := invalid_id;
                          vd_target_name := None;
                          vd_offset := old_offset;
                          vd_length := length;
                        |}
                      in
                      {|
                        vr_status := FiledOk;
                        vr_state := set_file_offset state file_id (old_offset + length);
                        vr_value := Some decision;
                        vr_decision := decision;
                      |}
                    else vfs_fail state FiledErrIsDir
                | None => vfs_fail state FiledErrInvalid
                end
            | None => vfs_fail state FiledErrInvalid
            end
        | _ => vfs_fail state FiledErrInvalid
        end
      else vfs_fail state FiledErrDenied
  | None => vfs_fail state FiledErrInvalid
  end.

Definition pread_prepare
    (state : vfs_state)
    (id : handle_id)
    (offset length : Z)
  : vfs_result vfs_decision :=
  match state_find_handle state id with
  | Some handle =>
      if andb (has_right VfsRightRead (vh_rights handle))
              (andb (z_nonnegative offset) (z_nonnegative length))
      then
        match vh_target handle with
        | HandleFile file_id =>
            match state_find_file state file_id with
            | Some file =>
                match state_find_vnode state (vf_vnode_id file) with
                | Some node =>
                    if vnode_is_regular node then
                      let decision :=
                        {|
                          vd_kind := VfsDecisionBackendRead;
                          vd_vnode_id := vn_id node;
                          vd_name := None;
                          vd_target_vnode_id := invalid_id;
                          vd_target_name := None;
                          vd_offset := offset;
                          vd_length := length;
                        |}
                      in
                      {|
                        vr_status := FiledOk;
                        vr_state := state;
                        vr_value := Some decision;
                        vr_decision := decision;
                      |}
                    else vfs_fail state FiledErrIsDir
                | None => vfs_fail state FiledErrInvalid
                end
            | None => vfs_fail state FiledErrInvalid
            end
        | _ => vfs_fail state FiledErrInvalid
        end
      else vfs_fail state FiledErrDenied
  | None => vfs_fail state FiledErrInvalid
  end.

Definition getdents_prepare
    (state : vfs_state)
    (id : handle_id)
  : vfs_result vfs_decision :=
  match state_find_handle state id with
  | Some handle =>
      if has_right VfsRightGetdents (vh_rights handle) then
        match vh_target handle with
        | HandleFile file_id =>
            match state_find_file state file_id with
            | Some file =>
                match state_find_vnode state (vf_vnode_id file) with
                | Some node =>
                    if vnode_is_directory node then
                      let decision :=
                        {|
                          vd_kind := VfsDecisionBackendGetdents;
                          vd_vnode_id := vn_id node;
                          vd_name := None;
                          vd_target_vnode_id := invalid_id;
                          vd_target_name := None;
                          vd_offset := 0;
                          vd_length := 0;
                        |}
                      in
                      {|
                        vr_status := FiledOk;
                        vr_state := state;
                        vr_value := Some decision;
                        vr_decision := decision;
                      |}
                    else vfs_fail state FiledErrNotDir
                | None => vfs_fail state FiledErrInvalid
                end
            | None => vfs_fail state FiledErrInvalid
            end
        | _ => vfs_fail state FiledErrInvalid
        end
      else vfs_fail state FiledErrDenied
  | None => vfs_fail state FiledErrInvalid
  end.
