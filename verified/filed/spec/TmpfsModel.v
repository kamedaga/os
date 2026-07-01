From Stdlib Require Import Bool.Bool Lia Lists.List Strings.Ascii Strings.String ZArith.ZArith.
From Pacha.Filed Require Import FiledTypes.

Import ListNotations.
Open Scope string_scope.
Open Scope Z_scope.

Definition tmpfs_object_id := backend_object_id.
Definition tmpfs_byte := Z.

Record tmpfs_node : Type := {
  tn_object : tmpfs_object_id;
  tn_parent : option tmpfs_object_id;
  tn_name : string;
  tn_kind : vnode_kind;
  tn_linked : bool;
  tn_generation : generation;
  tn_mode : Z;
  tn_data : list tmpfs_byte;
}.

Record tmpfs_state : Type := {
  ts_nodes : list tmpfs_node;
  ts_root : tmpfs_object_id;
  ts_next_object : tmpfs_object_id;
}.

Record tmpfs_result (A : Type) : Type := {
  tr_status : filed_status;
  tr_state : tmpfs_state;
  tr_value : option A;
}.

Arguments tr_status {A} _.
Arguments tr_state {A} _.
Arguments tr_value {A} _.

Definition tmpfs_ok
    {A : Type}
    (state : tmpfs_state)
    (value : A)
  : tmpfs_result A :=
  {|
    tr_status := FiledOk;
    tr_state := state;
    tr_value := Some value;
  |}.

Definition tmpfs_fail
    {A : Type}
    (state : tmpfs_state)
    (status : filed_status)
  : tmpfs_result A :=
  {|
    tr_status := status;
    tr_state := state;
    tr_value := None;
  |}.

Definition tmpfs_root_object : tmpfs_object_id := 1%nat.

Definition tmpfs_regular_mode (mode : Z) : Z :=
  32768 + Z.land mode 4095.

Definition tmpfs_directory_mode (mode : Z) : Z :=
  16384 + Z.land mode 4095.

Definition tmpfs_kind_mode (kind : vnode_kind) (mode : Z) : Z :=
  match kind with
  | VnodeDirectory => tmpfs_directory_mode mode
  | VnodeRegular => tmpfs_regular_mode mode
  | _ => mode
  end.

Definition mk_tmpfs_root : tmpfs_node :=
  {|
    tn_object := tmpfs_root_object;
    tn_parent := None;
    tn_name := "/";
    tn_kind := VnodeDirectory;
    tn_linked := true;
    tn_generation := 1%nat;
    tn_mode := tmpfs_directory_mode 493;
    tn_data := [];
  |}.

Definition empty_tmpfs_state : tmpfs_state :=
  {|
    ts_nodes := [mk_tmpfs_root];
    ts_root := tmpfs_root_object;
    ts_next_object := 2%nat;
  |}.

Fixpoint tmpfs_find_node
    (nodes : list tmpfs_node)
    (object : tmpfs_object_id)
  : option tmpfs_node :=
  match nodes with
  | [] => None
  | node :: rest =>
      if Nat.eqb (tn_object node) object then
        Some node
      else tmpfs_find_node rest object
  end.

Definition tmpfs_state_find
    (state : tmpfs_state)
    (object : tmpfs_object_id)
  : option tmpfs_node :=
  tmpfs_find_node (ts_nodes state) object.

Definition tmpfs_node_is_directory
    (node : tmpfs_node)
  : bool :=
  match tn_kind node with
  | VnodeDirectory => true
  | _ => false
  end.

Definition tmpfs_node_is_regular
    (node : tmpfs_node)
  : bool :=
  match tn_kind node with
  | VnodeRegular => true
  | _ => false
  end.

Definition tmpfs_slash : ascii := ascii_of_nat 47.

Fixpoint tmpfs_string_contains
    (needle : ascii)
    (text : string)
  : bool :=
  match text with
  | EmptyString => false
  | String head rest =>
      if ascii_dec head needle then true else tmpfs_string_contains needle rest
  end.

Definition tmpfs_valid_child_name
    (name : string)
  : bool :=
  match name with
  | EmptyString => false
  | _ =>
      negb
        (orb
          (orb (string_eqb name ".") (string_eqb name ".."))
          (tmpfs_string_contains tmpfs_slash name))
  end.

Definition tmpfs_child_matches
    (parent : tmpfs_object_id)
    (name : string)
    (node : tmpfs_node)
  : bool :=
  let parent_matches :=
    match tn_parent node with
    | Some actual => Nat.eqb actual parent
    | None => false
    end
  in
  andb
    (andb parent_matches (tn_linked node))
    (string_eqb (tn_name node) name).

Fixpoint tmpfs_lookup_child_in
    (nodes : list tmpfs_node)
    (parent : tmpfs_object_id)
    (name : string)
  : option tmpfs_node :=
  match nodes with
  | [] => None
  | node :: rest =>
      if tmpfs_child_matches parent name node then
        Some node
      else tmpfs_lookup_child_in rest parent name
  end.

Definition tmpfs_lookup_child
    (state : tmpfs_state)
    (parent : tmpfs_object_id)
    (name : string)
  : tmpfs_result tmpfs_object_id :=
  match tmpfs_state_find state parent with
  | None => tmpfs_fail state FiledErrNotFound
  | Some parent_node =>
      if tmpfs_node_is_directory parent_node then
        if string_eqb name "." then
          tmpfs_ok state parent
        else if string_eqb name ".." then
          match tn_parent parent_node with
          | Some parent_parent => tmpfs_ok state parent_parent
          | None => tmpfs_ok state parent
          end
        else
          match tmpfs_lookup_child_in (ts_nodes state) parent name with
          | Some child => tmpfs_ok state (tn_object child)
          | None => tmpfs_fail state FiledErrNotFound
          end
      else tmpfs_fail state FiledErrNotDir
  end.

Definition tmpfs_node_with_generation
    (node : tmpfs_node)
    (generation_value : generation)
  : tmpfs_node :=
  {|
    tn_object := tn_object node;
    tn_parent := tn_parent node;
    tn_name := tn_name node;
    tn_kind := tn_kind node;
    tn_linked := tn_linked node;
    tn_generation := generation_value;
    tn_mode := tn_mode node;
    tn_data := tn_data node;
  |}.

Definition tmpfs_bump_node_generation
    (node : tmpfs_node)
  : tmpfs_node :=
  tmpfs_node_with_generation node (S (tn_generation node)).

Definition tmpfs_node_with_linked
    (node : tmpfs_node)
    (linked : bool)
  : tmpfs_node :=
  {|
    tn_object := tn_object node;
    tn_parent := tn_parent node;
    tn_name := tn_name node;
    tn_kind := tn_kind node;
    tn_linked := linked;
    tn_generation := S (tn_generation node);
    tn_mode := tn_mode node;
    tn_data := tn_data node;
  |}.

Definition tmpfs_node_with_parent_name
    (node : tmpfs_node)
    (parent : tmpfs_object_id)
    (name : string)
  : tmpfs_node :=
  {|
    tn_object := tn_object node;
    tn_parent := Some parent;
    tn_name := name;
    tn_kind := tn_kind node;
    tn_linked := true;
    tn_generation := S (tn_generation node);
    tn_mode := tn_mode node;
    tn_data := tn_data node;
  |}.

Definition tmpfs_node_with_mode
    (node : tmpfs_node)
    (mode : Z)
  : tmpfs_node :=
  {|
    tn_object := tn_object node;
    tn_parent := tn_parent node;
    tn_name := tn_name node;
    tn_kind := tn_kind node;
    tn_linked := tn_linked node;
    tn_generation := S (tn_generation node);
    tn_mode := tmpfs_kind_mode (tn_kind node) mode;
    tn_data := tn_data node;
  |}.

Definition tmpfs_node_with_data
    (node : tmpfs_node)
    (data : list tmpfs_byte)
  : tmpfs_node :=
  {|
    tn_object := tn_object node;
    tn_parent := tn_parent node;
    tn_name := tn_name node;
    tn_kind := tn_kind node;
    tn_linked := tn_linked node;
    tn_generation := S (tn_generation node);
    tn_mode := tn_mode node;
    tn_data := data;
  |}.

Definition tmpfs_replace_node_entry
    (object : tmpfs_object_id)
    (replacement : tmpfs_node)
    (node : tmpfs_node)
  : tmpfs_node :=
  if Nat.eqb (tn_object node) object then replacement else node.

Definition tmpfs_replace_node
    (state : tmpfs_state)
    (object : tmpfs_object_id)
    (replacement : tmpfs_node)
  : tmpfs_state :=
  {|
    ts_nodes := map (tmpfs_replace_node_entry object replacement) (ts_nodes state);
    ts_root := ts_root state;
    ts_next_object := ts_next_object state;
  |}.

Definition tmpfs_insert_node
    (state : tmpfs_state)
    (node : tmpfs_node)
  : tmpfs_state :=
  {|
    ts_nodes := node :: ts_nodes state;
    ts_root := ts_root state;
    ts_next_object := S (ts_next_object state);
  |}.

Fixpoint tmpfs_has_linked_child_in
    (nodes : list tmpfs_node)
    (parent : tmpfs_object_id)
  : bool :=
  match nodes with
  | [] => false
  | node :: rest =>
      let parent_matches :=
        match tn_parent node with
        | Some actual => Nat.eqb actual parent
        | None => false
        end
      in
      if andb (tn_linked node) parent_matches then
        true
      else tmpfs_has_linked_child_in rest parent
  end.

Definition tmpfs_directory_empty
    (state : tmpfs_state)
    (directory : tmpfs_node)
  : bool :=
  negb (tmpfs_has_linked_child_in (ts_nodes state) (tn_object directory)).

Definition tmpfs_mk_node
    (object parent : tmpfs_object_id)
    (name : string)
    (kind : vnode_kind)
    (mode : Z)
  : tmpfs_node :=
  {|
    tn_object := object;
    tn_parent := Some parent;
    tn_name := name;
    tn_kind := kind;
    tn_linked := true;
    tn_generation := 1%nat;
    tn_mode := tmpfs_kind_mode kind mode;
    tn_data := [];
  |}.

Definition tmpfs_create_child
    (state : tmpfs_state)
    (parent : tmpfs_object_id)
    (name : string)
    (kind : vnode_kind)
    (mode : Z)
  : tmpfs_result tmpfs_object_id :=
  if tmpfs_valid_child_name name then
    match tmpfs_state_find state parent with
    | None => tmpfs_fail state FiledErrNotFound
    | Some parent_node =>
        if tmpfs_node_is_directory parent_node then
          match tmpfs_lookup_child_in (ts_nodes state) parent name with
          | Some _ => tmpfs_fail state FiledErrExists
          | None =>
              match kind with
              | VnodeRegular | VnodeDirectory =>
                  let object := ts_next_object state in
                  let child := tmpfs_mk_node object parent name kind mode in
                  let with_child := tmpfs_insert_node state child in
                  let with_parent_generation :=
                    tmpfs_replace_node
                      with_child
                      parent
                      (tmpfs_bump_node_generation parent_node)
                  in
                  tmpfs_ok with_parent_generation object
              | _ => tmpfs_fail state FiledErrUnsupported
              end
        else tmpfs_fail state FiledErrNotDir
    end
  else tmpfs_fail state FiledErrInvalid.

Definition tmpfs_create
    (state : tmpfs_state)
    (parent : tmpfs_object_id)
    (name : string)
    (mode : Z)
  : tmpfs_result tmpfs_object_id :=
  tmpfs_create_child state parent name VnodeRegular mode.

Definition tmpfs_mkdir
    (state : tmpfs_state)
    (parent : tmpfs_object_id)
    (name : string)
    (mode : Z)
  : tmpfs_result tmpfs_object_id :=
  tmpfs_create_child state parent name VnodeDirectory mode.

Definition tmpfs_padding
    (bytes : list tmpfs_byte)
    (offset : nat)
  : list tmpfs_byte :=
  repeat 0 (offset - length bytes)%nat.

Definition tmpfs_write_data
    (bytes : list tmpfs_byte)
    (offset : nat)
    (payload : list tmpfs_byte)
  : list tmpfs_byte :=
  let padded := bytes ++ tmpfs_padding bytes offset in
  firstn offset padded ++ payload ++ skipn (offset + length payload)%nat padded.

Definition tmpfs_truncate_data
    (bytes : list tmpfs_byte)
    (size : nat)
  : list tmpfs_byte :=
  firstn size bytes ++ repeat 0 (size - length bytes)%nat.

Definition tmpfs_pread
    (state : tmpfs_state)
    (object : tmpfs_object_id)
    (offset length : Z)
  : tmpfs_result (list tmpfs_byte) :=
  if andb (z_nonnegative offset) (z_nonnegative length) then
    match tmpfs_state_find state object with
    | None => tmpfs_fail state FiledErrNotFound
    | Some node =>
        if tmpfs_node_is_regular node then
          tmpfs_ok
            state
            (firstn (Z.to_nat length) (skipn (Z.to_nat offset) (tn_data node)))
        else if tmpfs_node_is_directory node then
          tmpfs_fail state FiledErrIsDir
        else tmpfs_fail state FiledErrInvalid
    end
  else tmpfs_fail state FiledErrInvalid.

Definition tmpfs_pwrite
    (state : tmpfs_state)
    (object : tmpfs_object_id)
    (offset : Z)
    (payload : list tmpfs_byte)
  : tmpfs_result Z :=
  if z_nonnegative offset then
    match tmpfs_state_find state object with
    | None => tmpfs_fail state FiledErrNotFound
    | Some node =>
        if tmpfs_node_is_regular node then
          let data := tmpfs_write_data (tn_data node) (Z.to_nat offset) payload in
          tmpfs_ok
            (tmpfs_replace_node state object (tmpfs_node_with_data node data))
            (Z.of_nat (length payload))
        else if tmpfs_node_is_directory node then
          tmpfs_fail state FiledErrIsDir
        else tmpfs_fail state FiledErrInvalid
    end
  else tmpfs_fail state FiledErrInvalid.

Definition tmpfs_truncate
    (state : tmpfs_state)
    (object : tmpfs_object_id)
    (size : Z)
  : tmpfs_result unit :=
  if z_nonnegative size then
    match tmpfs_state_find state object with
    | None => tmpfs_fail state FiledErrNotFound
    | Some node =>
        if tmpfs_node_is_regular node then
          let data := tmpfs_truncate_data (tn_data node) (Z.to_nat size) in
          tmpfs_ok
            (tmpfs_replace_node state object (tmpfs_node_with_data node data))
            tt
        else if tmpfs_node_is_directory node then
          tmpfs_fail state FiledErrIsDir
        else tmpfs_fail state FiledErrInvalid
    end
  else tmpfs_fail state FiledErrInvalid.

Definition tmpfs_chmod
    (state : tmpfs_state)
    (object : tmpfs_object_id)
    (mode : Z)
  : tmpfs_result unit :=
  match tmpfs_state_find state object with
  | None => tmpfs_fail state FiledErrNotFound
  | Some node =>
      tmpfs_ok
        (tmpfs_replace_node state object (tmpfs_node_with_mode node mode))
        tt
  end.

Definition tmpfs_mark_unlinked
    (state : tmpfs_state)
    (node : tmpfs_node)
  : tmpfs_state :=
  tmpfs_replace_node state (tn_object node) (tmpfs_node_with_linked node false).

Definition tmpfs_unlink
    (state : tmpfs_state)
    (parent : tmpfs_object_id)
    (name : string)
  : tmpfs_result unit :=
  if tmpfs_valid_child_name name then
    match tmpfs_state_find state parent with
    | None => tmpfs_fail state FiledErrNotFound
    | Some parent_node =>
        if tmpfs_node_is_directory parent_node then
          match tmpfs_lookup_child_in (ts_nodes state) parent name with
          | None => tmpfs_fail state FiledErrNotFound
          | Some child =>
              if tmpfs_node_is_directory child then
                tmpfs_fail state FiledErrIsDir
              else
                let unlinked := tmpfs_mark_unlinked state child in
                tmpfs_ok
                  (tmpfs_replace_node
                    unlinked
                    parent
                    (tmpfs_bump_node_generation parent_node))
                  tt
          end
        else tmpfs_fail state FiledErrNotDir
    end
  else tmpfs_fail state FiledErrInvalid.

Definition tmpfs_rmdir
    (state : tmpfs_state)
    (parent : tmpfs_object_id)
    (name : string)
  : tmpfs_result unit :=
  if tmpfs_valid_child_name name then
    match tmpfs_state_find state parent with
    | None => tmpfs_fail state FiledErrNotFound
    | Some parent_node =>
        if tmpfs_node_is_directory parent_node then
          match tmpfs_lookup_child_in (ts_nodes state) parent name with
          | None => tmpfs_fail state FiledErrNotFound
          | Some child =>
              if tmpfs_node_is_directory child then
                if tmpfs_directory_empty state child then
                  let unlinked := tmpfs_mark_unlinked state child in
                  tmpfs_ok
                    (tmpfs_replace_node
                      unlinked
                      parent
                      (tmpfs_bump_node_generation parent_node))
                    tt
                else tmpfs_fail state FiledErrNotEmpty
              else tmpfs_fail state FiledErrNotDir
          end
        else tmpfs_fail state FiledErrNotDir
    end
  else tmpfs_fail state FiledErrInvalid.

Definition tmpfs_rename_replace_target
    (state : tmpfs_state)
    (target : option tmpfs_node)
  : option tmpfs_state :=
  match target with
  | None => Some state
  | Some target_node =>
      if andb
          (tmpfs_node_is_directory target_node)
          (negb (tmpfs_directory_empty state target_node))
      then None
      else Some (tmpfs_mark_unlinked state target_node)
  end.

Definition tmpfs_rename
    (state : tmpfs_state)
    (old_parent : tmpfs_object_id)
    (old_name : string)
    (new_parent : tmpfs_object_id)
    (new_name : string)
  : tmpfs_result tmpfs_object_id :=
  if andb (tmpfs_valid_child_name old_name) (tmpfs_valid_child_name new_name) then
    match tmpfs_state_find state old_parent, tmpfs_state_find state new_parent with
    | Some old_parent_node, Some new_parent_node =>
        if andb
            (tmpfs_node_is_directory old_parent_node)
            (tmpfs_node_is_directory new_parent_node)
        then
          match tmpfs_lookup_child_in (ts_nodes state) old_parent old_name with
          | None => tmpfs_fail state FiledErrNotFound
          | Some source =>
              match tmpfs_lookup_child_in (ts_nodes state) new_parent new_name with
              | Some target =>
                  if Nat.eqb (tn_object source) (tn_object target) then
                    tmpfs_ok state (tn_object source)
                  else if andb
                    (tmpfs_node_is_directory target)
                    (negb (tmpfs_directory_empty state target))
                  then tmpfs_fail state FiledErrNotEmpty
                  else
                    let without_target := tmpfs_mark_unlinked state target in
                    let renamed :=
                      tmpfs_replace_node
                        without_target
                        (tn_object source)
                        (tmpfs_node_with_parent_name source new_parent new_name)
                    in
                    let bumped_old_parent :=
                      tmpfs_replace_node
                        renamed
                        old_parent
                        (tmpfs_bump_node_generation old_parent_node)
                    in
                    let bumped_new_parent :=
                      tmpfs_replace_node
                        bumped_old_parent
                        new_parent
                        (tmpfs_bump_node_generation new_parent_node)
                    in
                    tmpfs_ok bumped_new_parent (tn_object source)
              | None =>
                  let renamed :=
                    tmpfs_replace_node
                      state
                      (tn_object source)
                      (tmpfs_node_with_parent_name source new_parent new_name)
                  in
                  let bumped_old_parent :=
                    tmpfs_replace_node
                      renamed
                      old_parent
                      (tmpfs_bump_node_generation old_parent_node)
                  in
                  let bumped_new_parent :=
                    tmpfs_replace_node
                      bumped_old_parent
                      new_parent
                      (tmpfs_bump_node_generation new_parent_node)
                  in
                  tmpfs_ok bumped_new_parent (tn_object source)
              end
          end
        else tmpfs_fail state FiledErrNotDir
    | _, _ => tmpfs_fail state FiledErrNotFound
    end
  else tmpfs_fail state FiledErrInvalid.

Definition tmpfs_release_object
    (state : tmpfs_state)
    (object : tmpfs_object_id)
  : tmpfs_result unit :=
  match tmpfs_state_find state object with
  | None => tmpfs_fail state FiledErrNotFound
  | Some node =>
      if tn_linked node then
        tmpfs_ok state tt
      else
        tmpfs_ok
          {|
            ts_nodes :=
              filter
                (fun entry => negb (Nat.eqb (tn_object entry) object))
                (ts_nodes state);
            ts_root := ts_root state;
            ts_next_object := ts_next_object state;
          |}
          tt
  end.

Definition tmpfs_getdents
    (state : tmpfs_state)
    (directory : tmpfs_object_id)
    (offset capacity : nat)
  : tmpfs_result (list tmpfs_node) :=
  match tmpfs_state_find state directory with
  | None => tmpfs_fail state FiledErrNotFound
  | Some node =>
      if tmpfs_node_is_directory node then
        tmpfs_ok
          state
          (firstn
            capacity
            (skipn
              offset
              (filter
                (fun entry => tmpfs_child_matches directory (tn_name entry) entry)
                (ts_nodes state))))
      else tmpfs_fail state FiledErrNotDir
  end.
