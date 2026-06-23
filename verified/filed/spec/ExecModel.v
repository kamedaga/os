From Stdlib Require Import Bool.Bool Lists.List Strings.String ZArith.ZArith.
From Pacha.Filed Require Import FiledTypes VfsModel.

Import ListNotations.
Open Scope Z_scope.

Definition filed_page_size : Z := 4096.
Definition filed_stack_size : Z := 131072.
Definition filed_i64_max : Z := 9223372036854775807.

Definition elf_machine_x86_64 : Z := 62.
Definition elf_class_64 : Z := 2.
Definition elf_data_lsb : Z := 1.
Definition elf_version_current : Z := 1.

Inductive elf_type : Type :=
| ElfExec
| ElfDyn.

Inductive program_header_kind : Type :=
| PhLoad
| PhOther.

Record elf_header : Type := {
  eh_magic_ok : bool;
  eh_class : Z;
  eh_data : Z;
  eh_version : Z;
  eh_type : elf_type;
  eh_machine : Z;
  eh_entry : Z;
  eh_phoff : Z;
  eh_phentsize : Z;
  eh_phnum : Z;
}.

Record program_header : Type := {
  ph_kind : program_header_kind;
  ph_flags : Z;
  ph_offset : Z;
  ph_vaddr : Z;
  ph_filesz : Z;
  ph_memsz : Z;
  ph_align : Z;
}.

Record elf_image : Type := {
  ei_size : Z;
  ei_header : elf_header;
  ei_phdrs : list program_header;
}.

Record exec_request : Type := {
  er_path : list string;
  er_argv : list string;
  er_env : list string;
  er_argc : nat;
  er_envc : nat;
  er_bootstrap_fd : option fd_id;
  er_inherit_fds : list fd_id;
  er_cloexec_fds : list fd_id;
  er_aslr : bool;
}.

Definition exec_request_wf
    (request : exec_request)
  : Prop :=
  er_argc request = List.length (er_argv request) /\
  er_envc request = List.length (er_env request).

Record exec_mapping : Type := {
  em_vaddr : Z;
  em_memsz : Z;
  em_filesz : Z;
  em_offset : Z;
  em_prot : Z;
}.

Inductive auxv_key : Type :=
| AuxvPagesz
| AuxvEntry
| AuxvPhent
| AuxvPhnum
| AuxvBootstrapFd.

Definition auxv_key_eqb
    (lhs rhs : auxv_key)
  : bool :=
  match lhs, rhs with
  | AuxvPagesz, AuxvPagesz => true
  | AuxvEntry, AuxvEntry => true
  | AuxvPhent, AuxvPhent => true
  | AuxvPhnum, AuxvPhnum => true
  | AuxvBootstrapFd, AuxvBootstrapFd => true
  | _, _ => false
  end.

Record auxv_entry : Type := {
  ae_key : auxv_key;
  ae_value : Z;
}.

Record exec_stack_plan : Type := {
  esp_size : Z;
  esp_initial_sp : Z;
  esp_argc : nat;
  esp_envc : nat;
  esp_argv : list string;
  esp_env : list string;
  esp_auxv : list auxv_entry;
  esp_has_bootstrap : bool;
}.

Record exec_plan : Type := {
  ep_entry : Z;
  ep_load_bias : Z;
  ep_mappings : list exec_mapping;
  ep_stack : exec_stack_plan;
  ep_inherited_fds : list fd_id;
  ep_inherited_handles : list handle_id;
}.

Record exec_result : Type := {
  ex_status : filed_status;
  ex_plan : option exec_plan;
}.

Definition exec_ok
    (plan : exec_plan)
  : exec_result :=
  {|
    ex_status := FiledOk;
    ex_plan := Some plan;
  |}.

Definition exec_fail
    (status : filed_status)
  : exec_result :=
  {|
    ex_status := status;
    ex_plan := None;
  |}.

Definition is_supported_elf_type
    (kind : elf_type)
  : bool :=
  match kind with
  | ElfExec => true
  | ElfDyn => true
  end.

Definition validate_elf_header
    (image : elf_image)
  : bool :=
  let header := ei_header image in
  andb (eh_magic_ok header)
  (andb (eh_class header =? elf_class_64)
  (andb (eh_data header =? elf_data_lsb)
  (andb (eh_version header =? elf_version_current)
  (andb (eh_machine header =? elf_machine_x86_64)
  (andb (is_supported_elf_type (eh_type header))
  (andb (z_nonnegative (eh_entry header))
  (andb (z_nonnegative (eh_phoff header))
  (andb (z_positive (eh_phentsize header))
  (andb (z_nonnegative (eh_phnum header))
        ((eh_phoff header + eh_phentsize header * eh_phnum header) <=? ei_size image)))))))))).

Definition is_load_header
    (header : program_header)
  : bool :=
  match ph_kind header with
  | PhLoad => true
  | PhOther => false
  end.

Definition segment_file_range_valid
    (image : elf_image)
    (header : program_header)
  : bool :=
  andb (z_nonnegative (ph_offset header))
  (andb (z_nonnegative (ph_filesz header))
  (andb (z_nonnegative (ph_memsz header))
  (andb (ph_filesz header <=? ph_memsz header)
        ((ph_offset header + ph_filesz header) <=? ei_size image)))).

Definition segment_mem_range_valid
    (header : program_header)
  : bool :=
  andb (z_nonnegative (ph_vaddr header))
  (andb (z_positive (ph_memsz header))
        ((ph_vaddr header + ph_memsz header) <=? filed_i64_max)).

Definition valid_load_segment
    (image : elf_image)
    (header : program_header)
  : bool :=
  andb (is_load_header header)
       (andb (segment_file_range_valid image header)
             (segment_mem_range_valid header)).

Fixpoint collect_load_segments
    (image : elf_image)
    (headers : list program_header)
  : option (list program_header) :=
  match headers with
  | [] => Some []
  | header :: rest =>
      match ph_kind header with
      | PhLoad =>
          if valid_load_segment image header then
            match collect_load_segments image rest with
            | Some tail => Some (header :: tail)
            | None => None
            end
          else None
      | PhOther => collect_load_segments image rest
      end
  end.

Definition choose_load_bias
    (request : exec_request)
    (image : elf_image)
  : Z :=
  match eh_type (ei_header image) with
  | ElfExec => 0
  | ElfDyn => if er_aslr request then 268435456 else 0
  end.

Definition mapping_from_segment
    (bias : Z)
    (header : program_header)
  : exec_mapping :=
  {|
    em_vaddr := ph_vaddr header + bias;
    em_memsz := ph_memsz header;
    em_filesz := ph_filesz header;
    em_offset := ph_offset header;
    em_prot := ph_flags header;
  |}.

Definition build_mapping_plan
    (request : exec_request)
    (image : elf_image)
  : option (Z * list exec_mapping) :=
  match collect_load_segments image (ei_phdrs image) with
  | Some [] => None
  | Some segments =>
      let bias := choose_load_bias request image in
      Some (bias, map (mapping_from_segment bias) segments)
  | None => None
  end.

Definition fd_inherited
    (request : exec_request)
    (fd : fd_id)
  : bool :=
  negb (existsb (Nat.eqb fd) (er_cloexec_fds request)).

Definition build_fd_inherit_plan
    (request : exec_request)
  : list fd_id :=
  filter (fd_inherited request) (er_inherit_fds request).

Definition build_handle_inherit_plan
    (handles : list vfs_handle)
  : list handle_id :=
  map vh_id (filter handle_inheritable handles).

Definition auxv_entry_with_key
    (key : auxv_key)
    (entries : list auxv_entry)
  : bool :=
  existsb (fun entry => auxv_key_eqb (ae_key entry) key) entries.

Definition bootstrap_auxv
    (request : exec_request)
  : list auxv_entry :=
  match er_bootstrap_fd request with
  | Some fd =>
      [{| ae_key := AuxvBootstrapFd; ae_value := Z.of_nat fd |}]
  | None => []
  end.

Definition build_auxv_plan
    (request : exec_request)
    (image : elf_image)
  : list auxv_entry :=
  {|
    ae_key := AuxvPagesz;
    ae_value := filed_page_size;
  |} ::
  {|
    ae_key := AuxvEntry;
    ae_value := eh_entry (ei_header image);
  |} ::
  {|
    ae_key := AuxvPhent;
    ae_value := eh_phentsize (ei_header image);
  |} ::
  {|
    ae_key := AuxvPhnum;
    ae_value := eh_phnum (ei_header image);
  |} ::
  bootstrap_auxv request.

Definition build_stack_plan_for_image
    (request : exec_request)
    (image : elf_image)
  : exec_stack_plan :=
  {|
    esp_size := filed_stack_size;
    esp_initial_sp := filed_stack_size - 16;
    esp_argc := er_argc request;
    esp_envc := er_envc request;
    esp_argv := er_argv request;
    esp_env := er_env request;
    esp_auxv := build_auxv_plan request image;
    esp_has_bootstrap :=
      match er_bootstrap_fd request with
      | Some _ => true
      | None => false
      end;
  |}.

Definition build_stack_plan
    (request : exec_request)
  : exec_stack_plan :=
  build_stack_plan_for_image
    request
    {|
      ei_size := 0;
      ei_header :=
        {|
          eh_magic_ok := false;
          eh_class := 0;
          eh_data := 0;
          eh_version := 0;
          eh_type := ElfExec;
          eh_machine := 0;
          eh_entry := 0;
          eh_phoff := 0;
          eh_phentsize := 0;
          eh_phnum := 0;
        |};
      ei_phdrs := [];
    |}.

Definition build_exec_plan_with_handles
    (request : exec_request)
    (image : elf_image)
    (handles : list vfs_handle)
  : exec_result :=
  if validate_elf_header image then
    match build_mapping_plan request image with
    | Some (bias, mappings) =>
        exec_ok
          {|
            ep_entry := eh_entry (ei_header image) + bias;
            ep_load_bias := bias;
            ep_mappings := mappings;
            ep_stack := build_stack_plan_for_image request image;
            ep_inherited_fds := build_fd_inherit_plan request;
            ep_inherited_handles := build_handle_inherit_plan handles;
          |}
    | None => exec_fail FiledErrInvalidImage
    end
  else exec_fail FiledErrBadFormat.

Definition build_exec_plan
    (request : exec_request)
    (image : elf_image)
  : exec_result :=
  build_exec_plan_with_handles request image [].

Definition mapping_wf
    (mapping : exec_mapping)
  : Prop :=
  0 <= em_vaddr mapping /\
  0 < em_memsz mapping /\
  0 <= em_filesz mapping /\
  em_filesz mapping <= em_memsz mapping /\
  0 <= em_offset mapping.

Definition stack_plan_wf
    (stack : exec_stack_plan)
  : Prop :=
  0 < esp_size stack /\
  0 <= esp_initial_sp stack /\
  esp_initial_sp stack < esp_size stack.

Definition exec_plan_wf
    (plan : exec_plan)
  : Prop :=
  0 <= ep_entry plan /\
  0 <= ep_load_bias plan /\
  ep_mappings plan <> [] /\
  Forall mapping_wf (ep_mappings plan) /\
  stack_plan_wf (ep_stack plan) /\
  esp_argc (ep_stack plan) = List.length (esp_argv (ep_stack plan)) /\
  esp_envc (ep_stack plan) = List.length (esp_env (ep_stack plan)).
