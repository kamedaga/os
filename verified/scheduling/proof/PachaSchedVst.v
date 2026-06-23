From Stdlib Require Import Lists.List ZArith.ZArith.
From VST Require Import floyd.proofauto.
From Pacha.Scheduling Require Import
  ProtocolModel
  EevdfModel
  EevdfTransitions
  SchedRuntimeModel
  SchedRuntimeSpec.
From Pacha.Scheduling.Proof Require Import
  SchedRuntimeVstSpec.
From Pacha.Scheduling.Clight Require Import PachaSchedClight.

Import ListNotations.
Open Scope Z_scope.
Open Scope logic.

Module C := PachaSchedClight.

Module SchedClight.
  #[export] Instance CompSpecs : compspecs.
  Proof.
    make_compspecs C.prog.
  Defined.

  Definition Vprog : varspecs.
  Proof.
    mk_varspecs C.prog.
  Defined.

  Definition t_eevdf_entity : type :=
    Tstruct C._pacha_eevdf_entity noattr.

  Definition t_eevdf_runqueue : type :=
    Tstruct C._pacha_eevdf_runqueue noattr.

  Definition t_eevdf_pick_result : type :=
    Tstruct C._pacha_eevdf_pick_result noattr.

  Definition t_sched_cpu : type :=
    Tstruct C._pacha_sched_cpu noattr.

  Definition t_sched_decision : type :=
    Tstruct C._pacha_sched_decision noattr.

  Definition t_sched_state : type :=
    Tstruct C._pacha_sched_state noattr.

  Definition v_i64
      (value : Z)
    : val :=
    Vlong (Int64.repr value).

  Definition v_size
      (value : nat)
    : val :=
    Vlong (Int64.repr (Z.of_nat value)).

  Definition v_int
      (value : Z)
    : val :=
    Vint (Int.repr value).

  Definition v_bool
      (value : bool)
    : val :=
    v_int (if value then 1 else 0).

  Definition eevdf_state_c_val
      (state : eevdf_state)
    : val :=
    v_int (eevdf_state_c_value state).

  Definition eevdf_entity_data
      (entity : eevdf_entity)
    : reptype t_eevdf_entity :=
    (v_i64 (ee_thread_id entity),
     (v_i64 (ee_generation entity),
      (v_i64 (ee_weight entity),
       (v_i64 (ee_slice_ns entity),
        (v_i64 (ee_service_ns entity),
         (v_i64 (ee_vruntime entity),
          (v_i64 (ee_eligible_time entity),
           (v_i64 (ee_deadline entity),
            eevdf_state_c_val (ee_state entity))))))))).

  Definition eevdf_runqueue_data
      (rq : eevdf_runqueue)
    : reptype t_eevdf_runqueue :=
    (map eevdf_entity_data (er_entities rq),
     (v_size (er_entity_count rq),
      (v_size (er_runnable_count rq),
       (v_i64 (er_virtual_time rq),
        v_i64 (er_min_vruntime rq))))).

  Definition eevdf_empty_entity : eevdf_entity :=
    {|
      ee_thread_id := no_thread_id;
      ee_generation := 0;
      ee_weight := 0;
      ee_slice_ns := 0;
      ee_service_ns := 0;
      ee_vruntime := 0;
      ee_eligible_time := 0;
      ee_deadline := 0;
      ee_state := EEmpty;
    |}.

  Definition eevdf_pick_result_has_entity
      (result : pick_result)
    : bool :=
    match snd result with
    | Some _ => true
    | None => false
    end.

  Definition eevdf_pick_result_index
      (result : pick_result)
    : nat :=
    match snd result with
    | Some (index, _) => index
    | None => 0%nat
    end.

  Definition eevdf_pick_result_entity
      (result : pick_result)
    : eevdf_entity :=
    match snd result with
    | Some (_, entity) => entity
    | None => eevdf_empty_entity
    end.

  Definition eevdf_pick_result_data
      (result : pick_result)
    : reptype t_eevdf_pick_result :=
    (eevdf_runqueue_data (fst result),
     (v_bool (eevdf_pick_result_has_entity result),
      (v_size (eevdf_pick_result_index result),
       eevdf_entity_data (eevdf_pick_result_entity result)))).

  Definition sched_cpu_data
      (cpu : sched_cpu)
    : reptype t_sched_cpu :=
    (v_bool (sc_has_current cpu),
     (v_i64 (sc_current_thread_id cpu),
      v_i64 (sc_current_generation cpu))).

  Definition sched_decision_data
      (decision : sched_decision)
    : reptype t_sched_decision :=
    (v_int (sched_decision_kind_c_value (sd_kind decision)),
     (v_size (sd_cpu_id decision),
      (v_i64 (sd_thread_id decision),
       v_i64 (sd_generation decision)))).

  Definition sched_state_data
      (sched : sched_state)
    : reptype t_sched_state :=
    (eevdf_runqueue_data (ss_runqueue sched),
     (map sched_cpu_data (ss_cpus sched),
      v_size (ss_cpu_count sched))).

  Definition eevdf_runqueue_data_rep
      (sh : share)
      (rq : eevdf_runqueue)
      (ptr : val)
    : mpred :=
    data_at sh t_eevdf_runqueue (eevdf_runqueue_data rq) ptr.

  Definition eevdf_entity_data_rep
      (sh : share)
      (entity : eevdf_entity)
      (ptr : val)
    : mpred :=
    data_at sh t_eevdf_entity (eevdf_entity_data entity) ptr.

  Definition eevdf_pick_result_data_rep
      (sh : share)
      (result : pick_result)
      (ptr : val)
    : mpred :=
    data_at sh t_eevdf_pick_result (eevdf_pick_result_data result) ptr.

  Definition sched_state_data_rep
      (sh : share)
      (sched : sched_state)
      (ptr : val)
    : mpred :=
    data_at sh t_sched_state (sched_state_data sched) ptr.

  Definition sched_state_split_data_rep
      (sh : share)
      (sched : sched_state)
      (ptr : val)
    : mpred :=
    eevdf_runqueue_data_rep sh (ss_runqueue sched)
      (field_address t_sched_state [StructField C._runqueue] ptr) *
    field_at sh t_sched_state
      [StructField C._cpus]
      (map sched_cpu_data (ss_cpus sched)) ptr *
    field_at sh t_sched_state
      [StructField C._cpu_count]
      (v_size (ss_cpu_count sched)) ptr.

  Definition sched_decision_data_rep
      (sh : share)
      (decision : sched_decision)
      (ptr : val)
    : mpred :=
    data_at sh t_sched_decision (sched_decision_data decision) ptr.

  Definition sched_cpu_data_rep
      (sh : share)
      (cpu : sched_cpu)
      (ptr : val)
    : mpred :=
    data_at sh t_sched_cpu (sched_cpu_data cpu) ptr.

  Definition sched_cpu_array_element_ptr
      (sched_ptr : val)
      (cpu_id : nat)
    : val :=
    field_address t_sched_state
      [ArraySubsc (Z.of_nat cpu_id); StructField C._cpus]
      sched_ptr.

  Definition sched_cpu_field_rep
      (sh : share)
      (cpu : sched_cpu)
      (ptr : val)
    : mpred :=
    field_at sh t_sched_cpu
      [StructField C._has_current]
      (v_bool (sc_has_current cpu)) ptr *
    field_at sh t_sched_cpu
      [StructField C._current_thread_id]
      (v_i64 (sc_current_thread_id cpu)) ptr *
    field_at sh t_sched_cpu
      [StructField C._current_generation]
      (v_i64 (sc_current_generation cpu)) ptr.

  Definition sched_cpu_array_element_field_rep
      (sh : share)
      (cpu : sched_cpu)
      (sched_ptr : val)
      (cpu_id : nat)
    : mpred :=
    field_at sh t_sched_state
      [StructField C._has_current;
       ArraySubsc (Z.of_nat cpu_id);
       StructField C._cpus]
      (v_bool (sc_has_current cpu)) sched_ptr *
    field_at sh t_sched_state
      [StructField C._current_thread_id;
       ArraySubsc (Z.of_nat cpu_id);
       StructField C._cpus]
      (v_i64 (sc_current_thread_id cpu)) sched_ptr *
    field_at sh t_sched_state
      [StructField C._current_generation;
       ArraySubsc (Z.of_nat cpu_id);
       StructField C._cpus]
      (v_i64 (sc_current_generation cpu)) sched_ptr.

  Definition sched_cpu_array_element_rep
      (sh : share)
      (cpu : sched_cpu)
      (sched_ptr : val)
      (cpu_id : nat)
    : mpred :=
    field_at sh t_sched_state
      [ArraySubsc (Z.of_nat cpu_id); StructField C._cpus]
      (sched_cpu_data cpu) sched_ptr.

  Definition sched_decision_field_rep
      (sh : share)
      (decision : sched_decision)
      (ptr : val)
    : mpred :=
    field_at sh t_sched_decision
      [StructField C._kind]
      (v_int (sched_decision_kind_c_value (sd_kind decision))) ptr *
    field_at sh t_sched_decision
      [StructField C._cpu_id]
      (v_size (sd_cpu_id decision)) ptr *
    field_at sh t_sched_decision
      [StructField C._thread_id]
      (v_i64 (sd_thread_id decision)) ptr *
    field_at sh t_sched_decision
      [StructField C._generation]
      (v_i64 (sd_generation decision)) ptr.

  Definition pacha_sched_no_decision_spec : ident * funspec :=
    DECLARE C._pacha_sched_no_decision
    WITH out : val
    PRE [ tptr t_sched_decision ]
      PROP ()
      PARAMS (out)
      SEP (data_at_ Tsh t_sched_decision out)
    POST [ tvoid ]
      PROP ()
      RETURN ()
      SEP (sched_decision_data_rep Tsh sched_no_decision out).

  Definition map_eevdf_rc_spec : ident * funspec :=
    DECLARE C._map_eevdf_rc
    WITH rc : eevdf_rc
    PRE [ tint ]
      PROP ()
      PARAMS (v_int (eevdf_rc_c_value rc))
      SEP ()
    POST [ tint ]
      PROP ()
      RETURN (v_int (sched_rc_c_value (map_eevdf_rc rc)))
      SEP ().

  Definition idle_decision_spec : ident * funspec :=
    DECLARE C._idle_decision
    WITH cpu_id : nat, out : val
    PRE [ tulong, tptr t_sched_decision ]
      PROP (c_size_t_value (Z.of_nat cpu_id))
      PARAMS (v_size cpu_id; out)
      SEP (data_at_ Tsh t_sched_decision out)
    POST [ tvoid ]
      PROP ()
      RETURN ()
      SEP (sched_decision_data_rep Tsh (sched_idle_decision cpu_id) out).

  Definition run_thread_decision_spec : ident * funspec :=
    DECLARE C._run_thread_decision
    WITH cpu_id : nat, entity : eevdf_entity, entity_ptr : val, out : val
    PRE [ tulong, tptr t_eevdf_entity, tptr t_sched_decision ]
      PROP (
        c_size_t_value (Z.of_nat cpu_id);
        eevdf_entity_c_shape entity
      )
      PARAMS (v_size cpu_id; entity_ptr; out)
      SEP (
        eevdf_entity_data_rep Tsh entity entity_ptr;
        data_at_ Tsh t_sched_decision out
      )
    POST [ tvoid ]
      PROP ()
      RETURN ()
      SEP (
        eevdf_entity_data_rep Tsh entity entity_ptr;
        sched_decision_data_rep Tsh
          (sched_run_thread_decision cpu_id entity) out
      ).

  Definition valid_cpu_spec : ident * funspec :=
    DECLARE C._valid_cpu
    WITH sched_ptr : val, cpu_id : nat, cpu_count : nat
    PRE [ tptr t_sched_state, tulong ]
      PROP (
        c_size_t_value (Z.of_nat cpu_id);
        c_size_t_value (Z.of_nat cpu_count)
      )
      PARAMS (sched_ptr; v_size cpu_id)
      SEP (
        field_at Tsh t_sched_state
          [StructField C._cpu_count]
          (v_size cpu_count) sched_ptr
      )
    POST [ tint ]
      PROP ()
      RETURN (v_bool ((cpu_id <? cpu_count)%nat))
      SEP (
        field_at Tsh t_sched_state
          [StructField C._cpu_count]
          (v_size cpu_count) sched_ptr
      ).

  Definition sched_cpu_const_ptr_spec : ident * funspec :=
    DECLARE C._sched_cpu_const_ptr
    WITH sched_ptr : val, cpu_id : nat, cpus : list sched_cpu,
      cpu : sched_cpu
    PRE [ tptr t_sched_state, tulong ]
      PROP (
        c_size_t_value (Z.of_nat cpu_id);
        length cpus = sched_max_cpus;
        nth_error cpus cpu_id = Some cpu
      )
      PARAMS (sched_ptr; v_size cpu_id)
      SEP (
        field_at Tsh t_sched_state
          [StructField C._cpus]
          (map sched_cpu_data cpus) sched_ptr
      )
    POST [ tptr t_sched_cpu ]
      PROP ()
      RETURN (sched_cpu_array_element_ptr sched_ptr cpu_id)
      SEP (
        sched_cpu_data_rep Tsh cpu
          (sched_cpu_array_element_ptr sched_ptr cpu_id);
        sched_cpu_data_rep Tsh cpu
          (sched_cpu_array_element_ptr sched_ptr cpu_id) -*
        field_at Tsh t_sched_state
          [StructField C._cpus]
          (map sched_cpu_data cpus) sched_ptr
      ).

  Definition sched_cpu_ptr_spec : ident * funspec :=
    DECLARE C._sched_cpu_ptr
    WITH sched_ptr : val, cpu_id : nat, cpus : list sched_cpu,
      cpu : sched_cpu
    PRE [ tptr t_sched_state, tulong ]
      PROP (
        c_size_t_value (Z.of_nat cpu_id);
        length cpus = sched_max_cpus;
        nth_error cpus cpu_id = Some cpu
      )
      PARAMS (sched_ptr; v_size cpu_id)
      SEP (
        field_at Tsh t_sched_state
          [StructField C._cpus]
          (map sched_cpu_data cpus) sched_ptr
      )
    POST [ tptr t_sched_cpu ]
      PROP ()
      RETURN (sched_cpu_array_element_ptr sched_ptr cpu_id)
      SEP (
        sched_cpu_data_rep Tsh cpu
          (sched_cpu_array_element_ptr sched_ptr cpu_id);
        sched_cpu_data_rep Tsh cpu
          (sched_cpu_array_element_ptr sched_ptr cpu_id) -*
        field_at Tsh t_sched_state
          [StructField C._cpus]
          (map sched_cpu_data cpus) sched_ptr
      ).

  Definition sched_cpu_has_current_spec : ident * funspec :=
    DECLARE C._sched_cpu_has_current
    WITH cpu_ptr : val, cpu : sched_cpu
    PRE [ tptr t_sched_cpu ]
      PROP (sched_cpu_c_shape cpu)
      PARAMS (cpu_ptr)
      SEP (sched_cpu_data_rep Tsh cpu cpu_ptr)
    POST [ tint ]
      PROP ()
      RETURN (v_bool (sc_has_current cpu))
      SEP (sched_cpu_data_rep Tsh cpu cpu_ptr).

  Definition cpu_has_current_spec : ident * funspec :=
    DECLARE C._cpu_has_current
    WITH sched_ptr : val, cpu_id : nat, cpu_count : nat,
      cpus : list sched_cpu, cpu : sched_cpu
    PRE [ tptr t_sched_state, tulong ]
      PROP (
	        c_size_t_value (Z.of_nat cpu_id);
	        c_size_t_value (Z.of_nat cpu_count);
	        length cpus = sched_max_cpus;
	        nth_error cpus cpu_id = Some cpu;
	        sched_cpu_c_shape cpu
	      )
      PARAMS (sched_ptr; v_size cpu_id)
      SEP (
        field_at Tsh t_sched_state
          [StructField C._cpu_count]
          (v_size cpu_count) sched_ptr;
        field_at Tsh t_sched_state
          [StructField C._cpus]
          (map sched_cpu_data cpus) sched_ptr
      )
    POST [ tint ]
      PROP ()
      RETURN (v_bool
        (andb (cpu_id <? cpu_count)%nat (sc_has_current cpu)))
      SEP (
        field_at Tsh t_sched_state
          [StructField C._cpu_count]
          (v_size cpu_count) sched_ptr;
        field_at Tsh t_sched_state
          [StructField C._cpus]
          (map sched_cpu_data cpus) sched_ptr
      ).

  Definition clear_cpu_current_spec : ident * funspec :=
    DECLARE C._clear_cpu_current
    WITH cpu_ptr : val, old_cpu : sched_cpu
    PRE [ tptr t_sched_cpu ]
      PROP (sched_cpu_c_shape old_cpu)
      PARAMS (cpu_ptr)
      SEP (sched_cpu_data_rep Tsh old_cpu cpu_ptr)
    POST [ tvoid ]
      PROP ()
      RETURN ()
      SEP (sched_cpu_data_rep Tsh sched_empty_cpu cpu_ptr).

  Definition set_cpu_current_spec : ident * funspec :=
    DECLARE C._set_cpu_current
    WITH cpu_ptr : val, old_cpu : sched_cpu, thread_id : Z
    PRE [ tptr t_sched_cpu, tlong ]
      PROP (
        sched_cpu_c_shape old_cpu;
        c_int64_value thread_id
      )
      PARAMS (cpu_ptr; v_i64 thread_id)
      SEP (sched_cpu_data_rep Tsh old_cpu cpu_ptr)
    POST [ tvoid ]
      PROP ()
      RETURN ()
      SEP (sched_cpu_data_rep Tsh
        {|
          sc_has_current := true;
          sc_current_thread_id := thread_id;
        |} cpu_ptr).

  Definition clear_current_if_matches_spec : ident * funspec :=
    DECLARE C._clear_current_if_matches
    WITH sched : sched_state, sched_ptr : val, thread_id : Z
    PRE [ tptr t_sched_state, tlong ]
      PROP (sched_unary_thread_vst_pre sched thread_id)
      PARAMS (sched_ptr; v_i64 thread_id)
      SEP (
        eevdf_runqueue_data_rep Tsh (ss_runqueue sched)
          (field_address t_sched_state [StructField C._runqueue] sched_ptr);
        field_at Tsh t_sched_state
          [StructField C._cpus]
          (map sched_cpu_data (ss_cpus sched)) sched_ptr;
        field_at Tsh t_sched_state
          [StructField C._cpu_count]
          (v_size (ss_cpu_count sched)) sched_ptr
      )
    POST [ tvoid ]
      let after := clear_current_if_matches sched thread_id in
      PROP ()
      RETURN ()
      SEP (
        eevdf_runqueue_data_rep Tsh (ss_runqueue after)
          (field_address t_sched_state [StructField C._runqueue] sched_ptr);
        field_at Tsh t_sched_state
          [StructField C._cpus]
          (map sched_cpu_data (ss_cpus after)) sched_ptr;
        field_at Tsh t_sched_state
          [StructField C._cpu_count]
          (v_size (ss_cpu_count after)) sched_ptr
      ).

  Definition pacha_eevdf_empty_runqueue_spec : ident * funspec :=
    DECLARE C._pacha_eevdf_empty_runqueue
    WITH out : val
    PRE [ tptr t_eevdf_runqueue ]
      PROP ()
      PARAMS (out)
      SEP (data_at_ Tsh t_eevdf_runqueue out)
    POST [ tvoid ]
      PROP ()
      RETURN ()
      SEP (eevdf_runqueue_data_rep Tsh eevdf_empty_runqueue out).

  Definition pacha_eevdf_copy_runqueue_spec : ident * funspec :=
    DECLARE C._pacha_eevdf_copy_runqueue
    WITH src_rq : eevdf_runqueue, dst_rq : eevdf_runqueue,
      src : val, dst : val
    PRE [ tptr t_eevdf_runqueue, tptr t_eevdf_runqueue ]
      PROP ()
      PARAMS (src; dst)
      SEP (
        eevdf_runqueue_data_rep Tsh src_rq src;
        eevdf_runqueue_data_rep Tsh dst_rq dst
      )
    POST [ tvoid ]
      PROP ()
      RETURN ()
      SEP (
        eevdf_runqueue_data_rep Tsh src_rq src;
        eevdf_runqueue_data_rep Tsh src_rq dst
      ).

  Definition pacha_eevdf_add_spec : ident * funspec :=
    DECLARE C._pacha_eevdf_add
    WITH rq : eevdf_runqueue, rq_ptr : val,
      thread_id : Z, generation : Z, weight : Z, slice_ns : Z, out : val
    PRE [
      tptr t_eevdf_runqueue, tlong, tlong, tlong, tlong,
      tptr t_eevdf_runqueue
    ]
      PROP (eevdf_add_vst_pre rq thread_id generation weight slice_ns)
      PARAMS (
        rq_ptr; v_i64 thread_id; v_i64 generation; v_i64 weight;
        v_i64 slice_ns; out
      )
      SEP (
        eevdf_runqueue_data_rep Tsh rq rq_ptr;
        data_at_ Tsh t_eevdf_runqueue out
      )
    POST [ tint ]
      let result := eevdf_add rq thread_id generation weight slice_ns in
      PROP (eevdf_add_vst_post rq (eevdf_result_rq result)
        (eevdf_result_rc result) thread_id generation weight slice_ns)
      RETURN (v_int (eevdf_rc_c_value (eevdf_result_rc result)))
      SEP (
        eevdf_runqueue_data_rep Tsh rq rq_ptr;
        eevdf_runqueue_data_rep Tsh (eevdf_result_rq result) out
      ).

  Definition pacha_eevdf_wake_spec : ident * funspec :=
    DECLARE C._pacha_eevdf_wake
    WITH rq : eevdf_runqueue, rq_ptr : val, thread_id : Z, out : val
    PRE [ tptr t_eevdf_runqueue, tlong, tptr t_eevdf_runqueue ]
      PROP (eevdf_unary_thread_vst_pre rq thread_id)
      PARAMS (rq_ptr; v_i64 thread_id; out)
      SEP (
        eevdf_runqueue_data_rep Tsh rq rq_ptr;
        data_at_ Tsh t_eevdf_runqueue out
      )
    POST [ tint ]
      let result := eevdf_wake rq thread_id in
      PROP (eevdf_wake_vst_post rq (eevdf_result_rq result)
        (eevdf_result_rc result) thread_id)
      RETURN (v_int (eevdf_rc_c_value (eevdf_result_rc result)))
      SEP (
        eevdf_runqueue_data_rep Tsh rq rq_ptr;
        eevdf_runqueue_data_rep Tsh (eevdf_result_rq result) out
      ).

  Definition pacha_eevdf_block_spec : ident * funspec :=
    DECLARE C._pacha_eevdf_block
    WITH rq : eevdf_runqueue, rq_ptr : val, thread_id : Z, out : val
    PRE [ tptr t_eevdf_runqueue, tlong, tptr t_eevdf_runqueue ]
      PROP (eevdf_unary_thread_vst_pre rq thread_id)
      PARAMS (rq_ptr; v_i64 thread_id; out)
      SEP (
        eevdf_runqueue_data_rep Tsh rq rq_ptr;
        data_at_ Tsh t_eevdf_runqueue out
      )
    POST [ tint ]
      let result := eevdf_block rq thread_id in
      PROP (eevdf_block_vst_post rq (eevdf_result_rq result)
        (eevdf_result_rc result) thread_id)
      RETURN (v_int (eevdf_rc_c_value (eevdf_result_rc result)))
      SEP (
        eevdf_runqueue_data_rep Tsh rq rq_ptr;
        eevdf_runqueue_data_rep Tsh (eevdf_result_rq result) out
      ).

  Definition pacha_eevdf_exit_spec : ident * funspec :=
    DECLARE C._pacha_eevdf_exit
    WITH rq : eevdf_runqueue, rq_ptr : val, thread_id : Z, out : val
    PRE [ tptr t_eevdf_runqueue, tlong, tptr t_eevdf_runqueue ]
      PROP (eevdf_unary_thread_vst_pre rq thread_id)
      PARAMS (rq_ptr; v_i64 thread_id; out)
      SEP (
        eevdf_runqueue_data_rep Tsh rq rq_ptr;
        data_at_ Tsh t_eevdf_runqueue out
      )
    POST [ tint ]
      let result := eevdf_exit rq thread_id in
      PROP (eevdf_exit_vst_post rq (eevdf_result_rq result)
        (eevdf_result_rc result) thread_id)
      RETURN (v_int (eevdf_rc_c_value (eevdf_result_rc result)))
      SEP (
        eevdf_runqueue_data_rep Tsh rq rq_ptr;
        eevdf_runqueue_data_rep Tsh (eevdf_result_rq result) out
      ).

  Definition pacha_eevdf_charge_spec : ident * funspec :=
    DECLARE C._pacha_eevdf_charge
    WITH rq : eevdf_runqueue, rq_ptr : val,
      thread_id : Z, runtime_ns : Z, out : val
    PRE [ tptr t_eevdf_runqueue, tlong, tlong, tptr t_eevdf_runqueue ]
      PROP (eevdf_charge_vst_pre rq thread_id runtime_ns)
      PARAMS (rq_ptr; v_i64 thread_id; v_i64 runtime_ns; out)
      SEP (
        eevdf_runqueue_data_rep Tsh rq rq_ptr;
        data_at_ Tsh t_eevdf_runqueue out
      )
    POST [ tint ]
      let result := eevdf_charge rq thread_id runtime_ns in
      PROP (eevdf_charge_vst_post rq (eevdf_result_rq result)
        (eevdf_result_rc result) thread_id runtime_ns)
      RETURN (v_int (eevdf_rc_c_value (eevdf_result_rc result)))
      SEP (
        eevdf_runqueue_data_rep Tsh rq rq_ptr;
        eevdf_runqueue_data_rep Tsh (eevdf_result_rq result) out
      ).

  Definition pacha_eevdf_mark_running_spec : ident * funspec :=
    DECLARE C._pacha_eevdf_mark_running
    WITH rq : eevdf_runqueue, rq_ptr : val, thread_id : Z, out : val
    PRE [ tptr t_eevdf_runqueue, tlong, tptr t_eevdf_runqueue ]
      PROP (eevdf_unary_thread_vst_pre rq thread_id)
      PARAMS (rq_ptr; v_i64 thread_id; out)
      SEP (
        eevdf_runqueue_data_rep Tsh rq rq_ptr;
        data_at_ Tsh t_eevdf_runqueue out
      )
    POST [ tint ]
      let result := eevdf_mark_running rq thread_id in
      PROP (eevdf_mark_running_vst_post rq (eevdf_result_rq result)
        (eevdf_result_rc result) thread_id)
      RETURN (v_int (eevdf_rc_c_value (eevdf_result_rc result)))
      SEP (
        eevdf_runqueue_data_rep Tsh rq rq_ptr;
        eevdf_runqueue_data_rep Tsh (eevdf_result_rq result) out
      ).

  Definition pacha_eevdf_requeue_running_spec : ident * funspec :=
    DECLARE C._pacha_eevdf_requeue_running
    WITH rq : eevdf_runqueue, rq_ptr : val, thread_id : Z, out : val
    PRE [ tptr t_eevdf_runqueue, tlong, tptr t_eevdf_runqueue ]
      PROP (eevdf_unary_thread_vst_pre rq thread_id)
      PARAMS (rq_ptr; v_i64 thread_id; out)
      SEP (
        eevdf_runqueue_data_rep Tsh rq rq_ptr;
        data_at_ Tsh t_eevdf_runqueue out
      )
    POST [ tint ]
      let result := eevdf_requeue_running rq thread_id in
      PROP (eevdf_requeue_running_vst_post rq (eevdf_result_rq result)
        (eevdf_result_rc result) thread_id)
      RETURN (v_int (eevdf_rc_c_value (eevdf_result_rc result)))
      SEP (
        eevdf_runqueue_data_rep Tsh rq rq_ptr;
        eevdf_runqueue_data_rep Tsh (eevdf_result_rq result) out
      ).

  Definition pacha_eevdf_pick_spec : ident * funspec :=
    DECLARE C._pacha_eevdf_pick
    WITH rq : eevdf_runqueue, rq_ptr : val, out : val
    PRE [ tptr t_eevdf_runqueue, tptr t_eevdf_pick_result ]
      PROP (eevdf_runqueue_c_shape rq)
      PARAMS (rq_ptr; out)
      SEP (
        eevdf_runqueue_data_rep Tsh rq rq_ptr;
        data_at_ Tsh t_eevdf_pick_result out
      )
    POST [ tint ]
      let result := eevdf_pick rq in
      PROP (eevdf_runqueue_c_shape (fst result))
      RETURN (v_int (eevdf_rc_c_value EevdfOk))
      SEP (
        eevdf_runqueue_data_rep Tsh rq rq_ptr;
        eevdf_pick_result_data_rep Tsh result out
      ).

  Definition pacha_sched_add_thread_spec : ident * funspec :=
    DECLARE C._pacha_sched_add_thread
    WITH sched : sched_state, sched_ptr : val,
      thread_id : Z, generation : Z, weight : Z, slice_ns : Z,
      decision_out : val, scratch : val
    PRE [
      tptr t_sched_state, tlong, tlong, tlong, tlong,
      tptr t_sched_decision, tptr t_eevdf_runqueue
    ]
      PROP (
        sched_add_thread_vst_pre sched
          thread_id generation weight slice_ns;
        field_compatible t_sched_state
          [StructField C._runqueue] sched_ptr
      )
      PARAMS (
        sched_ptr; v_i64 thread_id; v_i64 generation;
        v_i64 weight; v_i64 slice_ns; decision_out; scratch
      )
      SEP (
        eevdf_runqueue_data_rep Tsh (ss_runqueue sched)
          (field_address t_sched_state [StructField C._runqueue] sched_ptr);
        field_at Tsh t_sched_state
          [StructField C._cpus]
          (map sched_cpu_data (ss_cpus sched)) sched_ptr;
        field_at Tsh t_sched_state
          [StructField C._cpu_count]
          (v_size (ss_cpu_count sched)) sched_ptr;
        data_at_ Tsh t_sched_decision decision_out;
        data_at_ Tsh t_eevdf_runqueue scratch
      )
    POST [ tint ]
      let result_pair :=
        sched_add_thread sched thread_id generation weight slice_ns in
      let after := fst result_pair in
      let result := snd result_pair in
      PROP (sched_add_thread_vst_post sched after
        (sr_rc result) (sr_decision result)
        thread_id generation weight slice_ns)
      RETURN (v_int (sched_rc_c_value (sr_rc result)))
      SEP (
        eevdf_runqueue_data_rep Tsh (ss_runqueue after)
          (field_address t_sched_state [StructField C._runqueue] sched_ptr);
        field_at Tsh t_sched_state
          [StructField C._cpus]
          (map sched_cpu_data (ss_cpus after)) sched_ptr;
        field_at Tsh t_sched_state
          [StructField C._cpu_count]
          (v_size (ss_cpu_count after)) sched_ptr;
        sched_decision_data_rep Tsh (sr_decision result) decision_out;
        eevdf_runqueue_data_rep Tsh (ss_runqueue after) scratch
      ).

  Definition pacha_sched_wake_thread_spec : ident * funspec :=
    DECLARE C._pacha_sched_wake_thread
    WITH sched : sched_state, sched_ptr : val,
      thread_id : Z, decision_out : val, scratch : val
    PRE [
      tptr t_sched_state, tlong, tptr t_sched_decision,
      tptr t_eevdf_runqueue
    ]
      PROP (
        sched_unary_thread_vst_pre sched thread_id;
        field_compatible t_sched_state
          [StructField C._runqueue] sched_ptr
      )
      PARAMS (sched_ptr; v_i64 thread_id; decision_out; scratch)
      SEP (
        eevdf_runqueue_data_rep Tsh (ss_runqueue sched)
          (field_address t_sched_state [StructField C._runqueue] sched_ptr);
        field_at Tsh t_sched_state
          [StructField C._cpus]
          (map sched_cpu_data (ss_cpus sched)) sched_ptr;
        field_at Tsh t_sched_state
          [StructField C._cpu_count]
          (v_size (ss_cpu_count sched)) sched_ptr;
        data_at_ Tsh t_sched_decision decision_out;
        data_at_ Tsh t_eevdf_runqueue scratch
      )
    POST [ tint ]
      let result_pair := sched_wake_thread sched thread_id in
      let after := fst result_pair in
      let result := snd result_pair in
      PROP (sched_wake_thread_vst_post sched after
        (sr_rc result) (sr_decision result) thread_id)
      RETURN (v_int (sched_rc_c_value (sr_rc result)))
      SEP (
        eevdf_runqueue_data_rep Tsh (ss_runqueue after)
          (field_address t_sched_state [StructField C._runqueue] sched_ptr);
        field_at Tsh t_sched_state
          [StructField C._cpus]
          (map sched_cpu_data (ss_cpus after)) sched_ptr;
        field_at Tsh t_sched_state
          [StructField C._cpu_count]
          (v_size (ss_cpu_count after)) sched_ptr;
        sched_decision_data_rep Tsh (sr_decision result) decision_out;
        eevdf_runqueue_data_rep Tsh (ss_runqueue after) scratch
      ).

  Definition pacha_sched_block_thread_spec : ident * funspec :=
    DECLARE C._pacha_sched_block_thread
    WITH sched : sched_state, sched_ptr : val,
      thread_id : Z, decision_out : val, scratch : val
    PRE [
      tptr t_sched_state, tlong, tptr t_sched_decision,
      tptr t_eevdf_runqueue
    ]
      PROP (
        sched_unary_thread_vst_pre sched thread_id;
        field_compatible t_sched_state
          [StructField C._runqueue] sched_ptr
      )
      PARAMS (sched_ptr; v_i64 thread_id; decision_out; scratch)
      SEP (
        eevdf_runqueue_data_rep Tsh (ss_runqueue sched)
          (field_address t_sched_state [StructField C._runqueue] sched_ptr);
        field_at Tsh t_sched_state
          [StructField C._cpus]
          (map sched_cpu_data (ss_cpus sched)) sched_ptr;
        field_at Tsh t_sched_state
          [StructField C._cpu_count]
          (v_size (ss_cpu_count sched)) sched_ptr;
        data_at_ Tsh t_sched_decision decision_out;
        data_at_ Tsh t_eevdf_runqueue scratch
      )
    POST [ tint ]
      let result_pair := sched_block_thread sched thread_id in
      let after := fst result_pair in
      let result := snd result_pair in
      PROP (sched_block_thread_vst_post sched after
        (sr_rc result) (sr_decision result) thread_id)
      RETURN (v_int (sched_rc_c_value (sr_rc result)))
      SEP (
        eevdf_runqueue_data_rep Tsh (ss_runqueue after)
          (field_address t_sched_state [StructField C._runqueue] sched_ptr);
        field_at Tsh t_sched_state
          [StructField C._cpus]
          (map sched_cpu_data (ss_cpus after)) sched_ptr;
        field_at Tsh t_sched_state
          [StructField C._cpu_count]
          (v_size (ss_cpu_count after)) sched_ptr;
        sched_decision_data_rep Tsh (sr_decision result) decision_out;
        eevdf_runqueue_data_rep Tsh (ss_runqueue after) scratch
      ).

  Definition pacha_sched_exit_thread_spec : ident * funspec :=
    DECLARE C._pacha_sched_exit_thread
    WITH sched : sched_state, sched_ptr : val,
      thread_id : Z, decision_out : val, scratch : val
    PRE [
      tptr t_sched_state, tlong, tptr t_sched_decision,
      tptr t_eevdf_runqueue
    ]
      PROP (
        sched_unary_thread_vst_pre sched thread_id;
        field_compatible t_sched_state
          [StructField C._runqueue] sched_ptr
      )
      PARAMS (sched_ptr; v_i64 thread_id; decision_out; scratch)
      SEP (
        eevdf_runqueue_data_rep Tsh (ss_runqueue sched)
          (field_address t_sched_state [StructField C._runqueue] sched_ptr);
        field_at Tsh t_sched_state
          [StructField C._cpus]
          (map sched_cpu_data (ss_cpus sched)) sched_ptr;
        field_at Tsh t_sched_state
          [StructField C._cpu_count]
          (v_size (ss_cpu_count sched)) sched_ptr;
        data_at_ Tsh t_sched_decision decision_out;
        data_at_ Tsh t_eevdf_runqueue scratch
      )
    POST [ tint ]
      let result_pair := sched_exit_thread sched thread_id in
      let after := fst result_pair in
      let result := snd result_pair in
      PROP (sched_exit_thread_vst_post sched after
        (sr_rc result) (sr_decision result) thread_id)
      RETURN (v_int (sched_rc_c_value (sr_rc result)))
      SEP (
        eevdf_runqueue_data_rep Tsh (ss_runqueue after)
          (field_address t_sched_state [StructField C._runqueue] sched_ptr);
        field_at Tsh t_sched_state
          [StructField C._cpus]
          (map sched_cpu_data (ss_cpus after)) sched_ptr;
        field_at Tsh t_sched_state
          [StructField C._cpu_count]
          (v_size (ss_cpu_count after)) sched_ptr;
        sched_decision_data_rep Tsh (sr_decision result) decision_out;
        eevdf_runqueue_data_rep Tsh (ss_runqueue after) scratch
      ).

  Definition sched_on_timer_scratch_rq
      (sched : sched_state)
      (cpu_id : nat)
      (runtime_ns : Z)
      (scratch_before : eevdf_runqueue)
    : eevdf_runqueue :=
    if valid_cpu sched cpu_id then
      match cpu_current_thread_id sched cpu_id with
      | Some thread_id =>
          eevdf_result_rq
            (eevdf_charge (ss_runqueue sched) thread_id runtime_ns)
      | None => scratch_before
      end
    else scratch_before.

  Definition on_timer_valid_spec : ident * funspec :=
    DECLARE C._on_timer_valid
    WITH sched : sched_state, sched_ptr : val,
      cpu_id : nat, runtime_ns : Z,
      scratch : val, scratch_before : eevdf_runqueue
    PRE [
      tptr t_sched_state, tulong, tlong, tptr t_eevdf_runqueue
    ]
      PROP (
        sched_timer_vst_pre sched cpu_id runtime_ns;
        valid_cpu sched cpu_id = true;
        field_compatible t_sched_state
          [StructField C._runqueue] sched_ptr
      )
      PARAMS (sched_ptr; v_size cpu_id; v_i64 runtime_ns; scratch)
      SEP (
        eevdf_runqueue_data_rep Tsh (ss_runqueue sched)
          (field_address t_sched_state [StructField C._runqueue] sched_ptr);
        field_at Tsh t_sched_state
          [StructField C._cpus]
          (map sched_cpu_data (ss_cpus sched)) sched_ptr;
        field_at Tsh t_sched_state
          [StructField C._cpu_count]
          (v_size (ss_cpu_count sched)) sched_ptr;
        eevdf_runqueue_data_rep Tsh scratch_before scratch
      )
    POST [ tint ]
      let result_pair := sched_on_timer sched cpu_id runtime_ns in
      let after := fst result_pair in
      let result := snd result_pair in
      PROP (sched_on_timer_vst_post sched after
        (sr_rc result) (sr_decision result) cpu_id runtime_ns)
      RETURN (v_int (sched_rc_c_value (sr_rc result)))
      SEP (
        eevdf_runqueue_data_rep Tsh (ss_runqueue after)
          (field_address t_sched_state [StructField C._runqueue] sched_ptr);
        field_at Tsh t_sched_state
          [StructField C._cpus]
          (map sched_cpu_data (ss_cpus after)) sched_ptr;
        field_at Tsh t_sched_state
          [StructField C._cpu_count]
          (v_size (ss_cpu_count after)) sched_ptr;
        eevdf_runqueue_data_rep Tsh
          (sched_on_timer_scratch_rq sched cpu_id runtime_ns scratch_before)
          scratch
      ).

  Definition pacha_sched_on_timer_spec : ident * funspec :=
    DECLARE C._pacha_sched_on_timer
    WITH sched : sched_state, sched_ptr : val,
      cpu_id : nat, runtime_ns : Z, decision_out : val,
      scratch : val, scratch_before : eevdf_runqueue
    PRE [
      tptr t_sched_state, tulong, tlong, tptr t_sched_decision,
      tptr t_eevdf_runqueue
    ]
      PROP (
        sched_timer_vst_pre sched cpu_id runtime_ns;
        field_compatible t_sched_state
          [StructField C._runqueue] sched_ptr
      )
      PARAMS (sched_ptr; v_size cpu_id; v_i64 runtime_ns;
        decision_out; scratch)
      SEP (
        eevdf_runqueue_data_rep Tsh (ss_runqueue sched)
          (field_address t_sched_state [StructField C._runqueue] sched_ptr);
        field_at Tsh t_sched_state
          [StructField C._cpus]
          (map sched_cpu_data (ss_cpus sched)) sched_ptr;
        field_at Tsh t_sched_state
          [StructField C._cpu_count]
          (v_size (ss_cpu_count sched)) sched_ptr;
        data_at_ Tsh t_sched_decision decision_out;
        eevdf_runqueue_data_rep Tsh scratch_before scratch
      )
    POST [ tint ]
      let result_pair := sched_on_timer sched cpu_id runtime_ns in
      let after := fst result_pair in
      let result := snd result_pair in
      PROP (sched_on_timer_vst_post sched after
        (sr_rc result) (sr_decision result) cpu_id runtime_ns)
      RETURN (v_int (sched_rc_c_value (sr_rc result)))
      SEP (
        eevdf_runqueue_data_rep Tsh (ss_runqueue after)
          (field_address t_sched_state [StructField C._runqueue] sched_ptr);
        field_at Tsh t_sched_state
          [StructField C._cpus]
          (map sched_cpu_data (ss_cpus after)) sched_ptr;
        field_at Tsh t_sched_state
          [StructField C._cpu_count]
          (v_size (ss_cpu_count after)) sched_ptr;
        sched_decision_data_rep Tsh (sr_decision result) decision_out;
        eevdf_runqueue_data_rep Tsh
          (sched_on_timer_scratch_rq sched cpu_id runtime_ns scratch_before)
          scratch
      ).

  Definition sched_finish_current_scratch_rq
      (sched : sched_state)
      (cpu_id : nat)
      (scratch_before : eevdf_runqueue)
    : eevdf_runqueue :=
    if valid_cpu sched cpu_id then
      match cpu_current_thread_id sched cpu_id with
      | Some thread_id =>
          eevdf_result_rq
            (eevdf_requeue_running (ss_runqueue sched) thread_id)
      | None => scratch_before
      end
    else scratch_before.

  Definition finish_current_valid_spec : ident * funspec :=
    DECLARE C._finish_current_valid
    WITH sched : sched_state, sched_ptr : val,
      cpu_id : nat, scratch : val, scratch_before : eevdf_runqueue
    PRE [
      tptr t_sched_state, tulong, tptr t_eevdf_runqueue
    ]
      PROP (
        sched_cpu_vst_pre sched cpu_id;
        valid_cpu sched cpu_id = true;
        field_compatible t_sched_state
          [StructField C._runqueue] sched_ptr
      )
      PARAMS (sched_ptr; v_size cpu_id; scratch)
      SEP (
        eevdf_runqueue_data_rep Tsh (ss_runqueue sched)
          (field_address t_sched_state [StructField C._runqueue] sched_ptr);
        field_at Tsh t_sched_state
          [StructField C._cpus]
          (map sched_cpu_data (ss_cpus sched)) sched_ptr;
        field_at Tsh t_sched_state
          [StructField C._cpu_count]
          (v_size (ss_cpu_count sched)) sched_ptr;
        eevdf_runqueue_data_rep Tsh scratch_before scratch
      )
    POST [ tint ]
      let result_pair := sched_finish_current sched cpu_id in
      let after := fst result_pair in
      let result := snd result_pair in
      PROP (sched_finish_current_vst_post sched after
        (sr_rc result) (sr_decision result) cpu_id)
      RETURN (v_int (sched_rc_c_value (sr_rc result)))
      SEP (
        eevdf_runqueue_data_rep Tsh (ss_runqueue after)
          (field_address t_sched_state [StructField C._runqueue] sched_ptr);
        field_at Tsh t_sched_state
          [StructField C._cpus]
          (map sched_cpu_data (ss_cpus after)) sched_ptr;
        field_at Tsh t_sched_state
          [StructField C._cpu_count]
          (v_size (ss_cpu_count after)) sched_ptr;
        eevdf_runqueue_data_rep Tsh
          (sched_finish_current_scratch_rq sched cpu_id scratch_before)
          scratch
      ).

  Definition pacha_sched_finish_current_spec : ident * funspec :=
    DECLARE C._pacha_sched_finish_current
    WITH sched : sched_state, sched_ptr : val,
      cpu_id : nat, decision_out : val,
      scratch : val, scratch_before : eevdf_runqueue
    PRE [
      tptr t_sched_state, tulong, tptr t_sched_decision,
      tptr t_eevdf_runqueue
    ]
      PROP (
        sched_cpu_vst_pre sched cpu_id;
        field_compatible t_sched_state
          [StructField C._runqueue] sched_ptr
      )
      PARAMS (sched_ptr; v_size cpu_id; decision_out; scratch)
      SEP (
        eevdf_runqueue_data_rep Tsh (ss_runqueue sched)
          (field_address t_sched_state [StructField C._runqueue] sched_ptr);
        field_at Tsh t_sched_state
          [StructField C._cpus]
          (map sched_cpu_data (ss_cpus sched)) sched_ptr;
        field_at Tsh t_sched_state
          [StructField C._cpu_count]
          (v_size (ss_cpu_count sched)) sched_ptr;
        data_at_ Tsh t_sched_decision decision_out;
        eevdf_runqueue_data_rep Tsh scratch_before scratch
      )
    POST [ tint ]
      let result_pair := sched_finish_current sched cpu_id in
      let after := fst result_pair in
      let result := snd result_pair in
      PROP (sched_finish_current_vst_post sched after
        (sr_rc result) (sr_decision result) cpu_id)
      RETURN (v_int (sched_rc_c_value (sr_rc result)))
      SEP (
        eevdf_runqueue_data_rep Tsh (ss_runqueue after)
          (field_address t_sched_state [StructField C._runqueue] sched_ptr);
        field_at Tsh t_sched_state
          [StructField C._cpus]
          (map sched_cpu_data (ss_cpus after)) sched_ptr;
        field_at Tsh t_sched_state
          [StructField C._cpu_count]
          (v_size (ss_cpu_count after)) sched_ptr;
        sched_decision_data_rep Tsh (sr_decision result) decision_out;
        eevdf_runqueue_data_rep Tsh
          (sched_finish_current_scratch_rq sched cpu_id scratch_before)
          scratch
      ).

  Definition sched_pick_scratch_rq
      (sched : sched_state)
      (cpu_id : nat)
      (scratch_before : eevdf_runqueue)
    : eevdf_runqueue :=
    if valid_cpu sched cpu_id then
      if cpu_has_current sched cpu_id then scratch_before
      else
        match snd (eevdf_pick (ss_runqueue sched)) with
        | Some (_, entity) =>
            eevdf_result_rq
              (eevdf_mark_running
                (fst (eevdf_pick (ss_runqueue sched)))
                (ee_thread_id entity))
        | None => scratch_before
        end
    else scratch_before.

  Definition sched_pick_result_scratch_rep
      (sched : sched_state)
      (cpu_id : nat)
      (pick_scratch : val)
    : mpred :=
    if valid_cpu sched cpu_id then
      if cpu_has_current sched cpu_id then
        data_at_ Tsh t_eevdf_pick_result pick_scratch
      else
        eevdf_pick_result_data_rep Tsh
          (eevdf_pick (ss_runqueue sched)) pick_scratch
    else data_at_ Tsh t_eevdf_pick_result pick_scratch.

  Definition pacha_sched_pick_spec : ident * funspec :=
    DECLARE C._pacha_sched_pick
    WITH sched : sched_state, sched_ptr : val,
      cpu_id : nat, decision_out : val, pick_scratch : val,
      scratch : val, scratch_before : eevdf_runqueue
    PRE [
      tptr t_sched_state, tulong, tptr t_sched_decision,
      tptr t_eevdf_pick_result, tptr t_eevdf_runqueue
    ]
      PROP (
        sched_cpu_vst_pre sched cpu_id;
        field_compatible t_sched_state
          [StructField C._runqueue] sched_ptr
      )
      PARAMS (sched_ptr; v_size cpu_id; decision_out; pick_scratch; scratch)
      SEP (
        eevdf_runqueue_data_rep Tsh (ss_runqueue sched)
          (field_address t_sched_state [StructField C._runqueue] sched_ptr);
        field_at Tsh t_sched_state
          [StructField C._cpus]
          (map sched_cpu_data (ss_cpus sched)) sched_ptr;
        field_at Tsh t_sched_state
          [StructField C._cpu_count]
          (v_size (ss_cpu_count sched)) sched_ptr;
        data_at_ Tsh t_sched_decision decision_out;
        data_at_ Tsh t_eevdf_pick_result pick_scratch;
        eevdf_runqueue_data_rep Tsh scratch_before scratch
      )
    POST [ tint ]
      let result_pair := sched_pick sched cpu_id in
      let after := fst result_pair in
      let result := snd result_pair in
      PROP (sched_pick_vst_post sched after
        (sr_rc result) (sr_decision result) cpu_id)
      RETURN (v_int (sched_rc_c_value (sr_rc result)))
      SEP (
        eevdf_runqueue_data_rep Tsh (ss_runqueue after)
          (field_address t_sched_state [StructField C._runqueue] sched_ptr);
        field_at Tsh t_sched_state
          [StructField C._cpus]
          (map sched_cpu_data (ss_cpus after)) sched_ptr;
        field_at Tsh t_sched_state
          [StructField C._cpu_count]
          (v_size (ss_cpu_count after)) sched_ptr;
        sched_decision_data_rep Tsh (sr_decision result) decision_out;
        sched_pick_result_scratch_rep sched cpu_id pick_scratch;
        eevdf_runqueue_data_rep Tsh
          (sched_pick_scratch_rq sched cpu_id scratch_before)
          scratch
      ).

  Definition Gprog : funspecs :=
    ltac:(with_library C.prog [
      pacha_eevdf_empty_runqueue_spec;
      pacha_eevdf_copy_runqueue_spec;
      pacha_eevdf_add_spec;
      pacha_eevdf_wake_spec;
      pacha_eevdf_block_spec;
      pacha_eevdf_exit_spec;
      pacha_eevdf_charge_spec;
      pacha_eevdf_mark_running_spec;
      pacha_eevdf_requeue_running_spec;
      pacha_eevdf_pick_spec;
      map_eevdf_rc_spec;
      pacha_sched_no_decision_spec;
      idle_decision_spec;
      run_thread_decision_spec;
      valid_cpu_spec;
      sched_cpu_const_ptr_spec;
      sched_cpu_ptr_spec;
      sched_cpu_has_current_spec;
      cpu_has_current_spec;
      clear_cpu_current_spec;
      set_cpu_current_spec;
      clear_current_if_matches_spec;
      pacha_sched_add_thread_spec;
      pacha_sched_wake_thread_spec;
      pacha_sched_block_thread_spec;
      pacha_sched_exit_thread_spec;
      on_timer_valid_spec;
      finish_current_valid_spec
    ]).
End SchedClight.

Existing Instance SchedClight.CompSpecs.

#[export] Instance sched_cpu_inhabitant : Inhabitant sched_cpu :=
  sched_empty_cpu.

Lemma Znth_of_nat_nth_error :
  forall {A : Type} `{Inhabitant A} (items : list A) index item,
    nth_error items index = Some item ->
    Znth (Z.of_nat index) items = item.
Proof.
  intros A Hinh items index item Hlookup.
  assert (Hlt : (index < length items)%nat).
  {
    apply nth_error_Some.
    rewrite Hlookup.
    discriminate.
  }
  pose proof (nth_error_nth A default index items Hlt) as Hnth.
  rewrite Hlookup in Hnth.
  inversion Hnth.
  rewrite <- nth_Znth'.
  reflexivity.
Qed.

Lemma eevdf_add_non_ok_runqueue :
  forall rq thread_id generation weight slice_ns,
    eevdf_result_rc
      (eevdf_add rq thread_id generation weight slice_ns) <> EevdfOk ->
    eevdf_result_rq
      (eevdf_add rq thread_id generation weight slice_ns) = rq.
Proof.
  intros rq thread_id generation weight slice_ns Hrc.
  unfold eevdf_add in Hrc |- *.
  destruct (thread_id =? no_thread_id); simpl; try reflexivity.
  destruct (negb (eevdf_valid_entity_params weight slice_ns)); simpl;
    try reflexivity.
  destruct (find_entity_index rq thread_id); simpl; try reflexivity.
  destruct (er_entity_count rq <? eevdf_max_entities)%nat; simpl;
    try reflexivity.
  destruct (eevdf_make_entity rq thread_id generation weight slice_ns);
    simpl; try reflexivity.
  exfalso.
  apply Hrc.
  reflexivity.
Qed.

Lemma eevdf_wake_non_ok_runqueue :
  forall rq thread_id,
    eevdf_result_rc (eevdf_wake rq thread_id) <> EevdfOk ->
    eevdf_result_rq (eevdf_wake rq thread_id) = rq.
Proof.
  intros rq thread_id Hrc.
  unfold eevdf_wake in Hrc |- *.
  destruct (find_entity_index rq thread_id); simpl; try reflexivity.
  destruct (lookup_entity rq n); simpl; try reflexivity.
  destruct (ee_state e); simpl; try reflexivity.
  destruct (refresh_deadline
    (set_entity_state (place_entity_at_floor e (er_min_vruntime rq))
      ERunnable) (er_min_vruntime rq)); simpl; try reflexivity.
  exfalso.
  apply Hrc.
  reflexivity.
Qed.

Lemma eevdf_block_non_ok_runqueue :
  forall rq thread_id,
    eevdf_result_rc (eevdf_block rq thread_id) <> EevdfOk ->
    eevdf_result_rq (eevdf_block rq thread_id) = rq.
Proof.
  intros rq thread_id Hrc.
  unfold eevdf_block in Hrc |- *.
  destruct (find_entity_index rq thread_id); simpl; try reflexivity.
  destruct (lookup_entity rq n); simpl; try reflexivity.
  destruct (runnable_or_running e); simpl; try reflexivity.
  exfalso.
  apply Hrc.
  reflexivity.
Qed.

Lemma eevdf_exit_non_ok_runqueue :
  forall rq thread_id,
    eevdf_result_rc (eevdf_exit rq thread_id) <> EevdfOk ->
    eevdf_result_rq (eevdf_exit rq thread_id) = rq.
Proof.
  intros rq thread_id Hrc.
  unfold eevdf_exit in Hrc |- *.
  destruct (find_entity_index rq thread_id); simpl; try reflexivity.
  destruct (lookup_entity rq n); simpl; try reflexivity.
  destruct (ee_state e); simpl; try reflexivity.
  all: exfalso; apply Hrc; reflexivity.
Qed.

Lemma eevdf_charge_non_ok_runqueue :
  forall rq thread_id runtime_ns,
    eevdf_result_rc (eevdf_charge rq thread_id runtime_ns) <> EevdfOk ->
    eevdf_result_rq (eevdf_charge rq thread_id runtime_ns) = rq.
Proof.
  intros rq thread_id runtime_ns Hrc.
  unfold eevdf_charge in Hrc |- *.
  destruct (find_entity_index rq thread_id); simpl; try reflexivity.
  destruct (lookup_entity rq n); simpl; try reflexivity.
  destruct (runnable_or_running e); simpl; try reflexivity.
  destruct (weighted_delta runtime_ns (ee_weight e)); simpl; try reflexivity.
  destruct (i64_value (ee_vruntime e + z)); simpl; try reflexivity.
  destruct (refresh_deadline
    {|
      ee_thread_id := ee_thread_id e;
      ee_generation := ee_generation e;
      ee_weight := ee_weight e;
      ee_slice_ns := ee_slice_ns e;
      ee_service_ns := ee_service_ns e + runtime_ns;
      ee_vruntime := ee_vruntime e + z;
      ee_eligible_time := ee_eligible_time e;
      ee_deadline := ee_deadline e;
      ee_state := ee_state e;
    |} (er_min_vruntime rq)); simpl; try reflexivity.
  exfalso.
  apply Hrc.
  reflexivity.
Qed.

Lemma sched_fail_map_eevdf_rc_c_shape :
  forall rc,
    sched_result_c_shape (sched_fail (map_eevdf_rc rc)).
Proof.
  intros rc.
  unfold sched_result_c_shape, sched_fail.
  split.
  - destruct rc;
      unfold c_int64_value, c_int64_min, c_int64_max,
        sched_rc_c_value, map_eevdf_rc;
      simpl; lia.
  - apply sched_no_decision_c_shape.
Qed.

Lemma sched_fail_c_shape :
  forall rc,
    sched_result_c_shape (sched_fail rc).
Proof.
  intros rc.
  unfold sched_result_c_shape, sched_fail.
  split.
  - destruct rc;
      unfold c_int64_value, c_int64_min, c_int64_max,
        sched_rc_c_value;
      simpl; lia.
  - apply sched_no_decision_c_shape.
Qed.

Lemma eevdf_rc_c_repr_zero_ok :
  forall rc,
    Int.repr (eevdf_rc_c_value rc) = Int.repr 0 ->
    rc = EevdfOk.
Proof.
  intros rc Hrepr.
  destruct rc; simpl in Hrepr; try reflexivity; discriminate Hrepr.
Qed.

Lemma sched_ok_no_decision_c_shape :
  sched_result_c_shape (sched_ok sched_no_decision).
Proof.
  unfold sched_result_c_shape, sched_ok.
  split.
  - unfold c_int64_value, c_int64_min, c_int64_max, sched_rc_c_value.
    simpl.
    lia.
  - apply sched_no_decision_c_shape.
Qed.

Lemma sched_state_valid_cpu_lookup :
  forall sched cpu_id,
    sched_state_c_shape sched ->
    valid_cpu sched cpu_id = true ->
    exists cpu,
      nth_error (ss_cpus sched) cpu_id = Some cpu /\
      sched_cpu_c_shape cpu.
Proof.
  intros sched cpu_id Hshape Hvalid.
  unfold sched_state_c_shape in Hshape.
  destruct Hshape as [_ [Hcpus_len [Hcpus_shape [_ Hcpu_count_bound]]]].
  unfold valid_cpu in Hvalid.
  apply Nat.ltb_lt in Hvalid.
  assert (Hcpu_lt_cpus : (cpu_id < length (ss_cpus sched))%nat)
    by lia.
  destruct (nth_error (ss_cpus sched) cpu_id) as [cpu |] eqn:Hlookup.
  - exists cpu.
    split; [reflexivity |].
    eapply Forall_forall.
    + exact Hcpus_shape.
    + eapply nth_error_In.
      exact Hlookup.
  - apply nth_error_None in Hlookup.
    lia.
Qed.

Lemma sched_on_timer_valid_decision_no_decision :
  forall sched cpu_id runtime_ns,
    valid_cpu sched cpu_id = true ->
    sr_decision (snd (sched_on_timer sched cpu_id runtime_ns)) =
      sched_no_decision.
Proof.
  intros sched cpu_id runtime_ns Hvalid.
  unfold sched_on_timer, sched_apply_eevdf_result.
  rewrite Hvalid.
  destruct (cpu_current_thread_id sched cpu_id) as [thread_id |].
  - destruct (eevdf_charge (ss_runqueue sched) thread_id runtime_ns)
      as [rc rq].
    destruct rc; reflexivity.
  - reflexivity.
Qed.

Lemma sched_finish_current_valid_decision_no_decision :
  forall sched cpu_id,
    valid_cpu sched cpu_id = true ->
    sr_decision (snd (sched_finish_current sched cpu_id)) =
      sched_no_decision.
Proof.
  intros sched cpu_id Hvalid.
  unfold sched_finish_current.
  rewrite Hvalid.
  destruct (cpu_current_thread_id sched cpu_id) as [thread_id |].
  - destruct (eevdf_requeue_running (ss_runqueue sched) thread_id)
      as [rc rq].
    destruct rc; reflexivity.
  - reflexivity.
Qed.

Lemma int64_ltu_size_nat_true :
  forall lhs rhs,
    c_size_t_value (Z.of_nat lhs) ->
    c_size_t_value (Z.of_nat rhs) ->
    (lhs <? rhs)%nat = true ->
    Int64.ltu (Int64.repr (Z.of_nat lhs))
      (Int64.repr (Z.of_nat rhs)) = true.
Proof.
  intros lhs rhs Hlhs Hrhs Hlt.
  apply Nat.ltb_lt in Hlt.
  unfold Int64.ltu.
  rewrite !Int64.unsigned_repr by
    (change Int64.max_unsigned with 18446744073709551615;
     unfold c_size_t_value, c_size_t_max in *; lia).
  destruct (zlt (Z.of_nat lhs) (Z.of_nat rhs)).
  - reflexivity.
  - lia.
Qed.

Lemma int64_ltu_size_nat_false :
  forall lhs rhs,
    c_size_t_value (Z.of_nat lhs) ->
    c_size_t_value (Z.of_nat rhs) ->
    (lhs <? rhs)%nat = false ->
    Int64.ltu (Int64.repr (Z.of_nat lhs))
      (Int64.repr (Z.of_nat rhs)) = false.
Proof.
  intros lhs rhs Hlhs Hrhs Hlt.
  apply Nat.ltb_ge in Hlt.
  unfold Int64.ltu.
  rewrite !Int64.unsigned_repr by
    (change Int64.max_unsigned with 18446744073709551615;
     unfold c_size_t_value, c_size_t_max in *; lia).
  destruct (zlt (Z.of_nat lhs) (Z.of_nat rhs)).
  - lia.
  - reflexivity.
Qed.

Lemma replace_nth_as_upd_Znth :
  forall {A : Type} (items : list A) index item,
    (index < length items)%nat ->
    ProtocolModel.replace_nth items index item =
      upd_Znth (Z.of_nat index) items item.
Proof.
  intros A items.
  induction items as [| head rest IH]; intros index item Hlt.
  - inversion Hlt.
  - simpl in Hlt.
    destruct index as [| index'].
    + rewrite upd_Znth0.
      reflexivity.
    + simpl.
      rewrite (IH index' item) by lia.
      rewrite upd_Znth_cons by lia.
      replace (Z.pos (Pos.of_succ_nat index') - 1)
        with (Z.of_nat index') by lia.
      reflexivity.
Qed.

Lemma map_replace_nth_sched_cpu_data :
  forall cpus index cpu,
    map SchedClight.sched_cpu_data
      (ProtocolModel.replace_nth cpus index cpu) =
      ProtocolModel.replace_nth (map SchedClight.sched_cpu_data cpus)
        index (SchedClight.sched_cpu_data cpu).
Proof.
  induction cpus as [| head rest IH]; intros index cpu.
  - destruct index; reflexivity.
  - destruct index as [| index']; simpl.
    + reflexivity.
    + rewrite IH.
      reflexivity.
Qed.

Lemma sched_cpu_data_repeat_empty :
  map SchedClight.sched_cpu_data
    (repeat sched_empty_cpu sched_max_cpus) =
  repeat (SchedClight.sched_cpu_data sched_empty_cpu) sched_max_cpus.
Proof.
  apply map_repeat.
Qed.

Lemma sched_cpu_array_update_data :
  forall cpus index cpu,
    (index < length cpus)%nat ->
    map SchedClight.sched_cpu_data
      (ProtocolModel.replace_nth cpus index cpu) =
      upd_Znth (Z.of_nat index)
        (map SchedClight.sched_cpu_data cpus)
        (SchedClight.sched_cpu_data cpu).
Proof.
  intros cpus index cpu Hlt.
  rewrite map_replace_nth_sched_cpu_data.
  rewrite replace_nth_as_upd_Znth.
  - reflexivity.
  - rewrite length_map.
    exact Hlt.
Qed.

Lemma sched_empty_cpu_array_update_data :
  forall index cpu,
    (index < sched_max_cpus)%nat ->
    map SchedClight.sched_cpu_data
      (ProtocolModel.replace_nth
        (repeat sched_empty_cpu sched_max_cpus) index cpu) =
      upd_Znth (Z.of_nat index)
        (repeat (SchedClight.sched_cpu_data sched_empty_cpu)
          sched_max_cpus)
        (SchedClight.sched_cpu_data cpu).
Proof.
  intros index cpu Hlt.
  rewrite sched_cpu_array_update_data by
    (rewrite repeat_length; exact Hlt).
  rewrite sched_cpu_data_repeat_empty.
  reflexivity.
Qed.

Lemma sched_cpu_array_element_rep_unfold :
  forall sh cpu sched_ptr cpu_id,
    SchedClight.sched_cpu_array_element_rep sh cpu sched_ptr cpu_id =
    field_at sh SchedClight.t_sched_state
      [ArraySubsc (Z.of_nat cpu_id); StructField C._cpus]
      (SchedClight.sched_cpu_data cpu) sched_ptr.
Proof.
  reflexivity.
Qed.

Lemma sched_cpu_array_element_field_rep_unfold :
  forall sh cpu sched_ptr cpu_id,
    SchedClight.sched_cpu_array_element_field_rep sh cpu sched_ptr cpu_id =
    field_at sh SchedClight.t_sched_state
      [StructField C._has_current;
       ArraySubsc (Z.of_nat cpu_id);
       StructField C._cpus]
      (SchedClight.v_bool (sc_has_current cpu)) sched_ptr *
    field_at sh SchedClight.t_sched_state
      [StructField C._current_thread_id;
       ArraySubsc (Z.of_nat cpu_id);
       StructField C._cpus]
      (SchedClight.v_i64 (sc_current_thread_id cpu)) sched_ptr.
Proof.
  reflexivity.
Qed.

Lemma sched_cpu_array_field_at_data_at :
  forall sh cpus sched_ptr,
    field_at sh SchedClight.t_sched_state
      [StructField C._cpus]
      (map SchedClight.sched_cpu_data cpus) sched_ptr =
    data_at sh (Tarray SchedClight.t_sched_cpu 256 noattr)
      (map SchedClight.sched_cpu_data cpus)
      (field_address SchedClight.t_sched_state
        [StructField C._cpus] sched_ptr).
Proof.
  intros sh cpus sched_ptr.
  rewrite field_at_data_at.
  change (nested_field_type SchedClight.t_sched_state
    [StructField C._cpus])
    with (Tarray SchedClight.t_sched_cpu 256 noattr).
  reflexivity.
Qed.

Lemma sched_cpu_array_subscript_offset :
  forall cpu_id,
    nested_field_offset
      (nested_field_type SchedClight.t_sched_state
        [StructField C._cpus])
      [ArraySubsc (Z.of_nat cpu_id)] =
    (sizeof SchedClight.t_sched_cpu * Z.of_nat cpu_id)%Z.
Proof.
  intros cpu_id.
  simpl.
  rewrite Z.add_0_l.
  reflexivity.
Qed.

Lemma sched_cpu_array_element_address :
  forall sched_ptr cpu_id,
    field_compatible SchedClight.t_sched_state
      [StructField C._cpus] sched_ptr ->
    field_compatible
      (nested_field_type SchedClight.t_sched_state
        [StructField C._cpus])
      [ArraySubsc (Z.of_nat cpu_id)]
      (field_address SchedClight.t_sched_state
        [StructField C._cpus] sched_ptr) ->
    field_address SchedClight.t_sched_state
      [ArraySubsc (Z.of_nat cpu_id); StructField C._cpus] sched_ptr =
    offset_val (sizeof SchedClight.t_sched_cpu * Z.of_nat cpu_id)%Z
      (field_address SchedClight.t_sched_state
        [StructField C._cpus] sched_ptr).
Proof.
  intros sched_ptr cpu_id Hcpus_compat Harray_compat.
  rewrite field_address_app
    with (gfsA := [StructField C._cpus])
         (gfsB := [ArraySubsc (Z.of_nat cpu_id)]).
  rewrite field_address_offset by exact Harray_compat.
  rewrite sched_cpu_array_subscript_offset.
  reflexivity.
Qed.

Lemma sched_cpu_array_element_sem_add :
  forall sched_ptr cpu_id,
    field_compatible SchedClight.t_sched_state
      [StructField C._cpus] sched_ptr ->
    field_compatible
      (nested_field_type SchedClight.t_sched_state
        [StructField C._cpus])
      [ArraySubsc (Z.of_nat cpu_id)]
      (field_address SchedClight.t_sched_state
        [StructField C._cpus] sched_ptr) ->
    force_val
      (sem_add_ptr_long SchedClight.t_sched_cpu
        (field_address SchedClight.t_sched_state
          [StructField C._cpus] sched_ptr)
        (SchedClight.v_size cpu_id)) =
    field_address SchedClight.t_sched_state
      [ArraySubsc (Z.of_nat cpu_id); StructField C._cpus] sched_ptr.
Proof.
  intros sched_ptr cpu_id Hcpus_compat Harray_compat.
  unfold SchedClight.v_size.
  rewrite sem_add_pl_ptr_special.
  - rewrite sched_cpu_array_element_address by eassumption.
    reflexivity.
  - cbv.
    reflexivity.
  - apply field_address_isptr.
    exact Hcpus_compat.
Qed.

Lemma sched_cpu_array_element_ramif :
  forall sh cpus cpu sched_ptr cpu_id,
    length cpus = sched_max_cpus ->
    nth_error cpus cpu_id = Some cpu ->
    (cpu_id < sched_max_cpus)%nat ->
    field_at sh SchedClight.t_sched_state
      [StructField C._cpus]
      (map SchedClight.sched_cpu_data cpus) sched_ptr |--
    SchedClight.sched_cpu_data_rep sh cpu
      (SchedClight.sched_cpu_array_element_ptr sched_ptr cpu_id) *
    (SchedClight.sched_cpu_data_rep sh cpu
      (SchedClight.sched_cpu_array_element_ptr sched_ptr cpu_id) -*
     field_at sh SchedClight.t_sched_state
      [StructField C._cpus]
      (map SchedClight.sched_cpu_data cpus) sched_ptr).
Proof.
  intros sh cpus cpu sched_ptr cpu_id Hlen Hlookup Hlt.
  assert (Hfield_array :
    field_at sh SchedClight.t_sched_state
      [StructField C._cpus]
      (map SchedClight.sched_cpu_data cpus) sched_ptr =
    array_at sh SchedClight.t_sched_state
      [StructField C._cpus] 0 256
      (map SchedClight.sched_cpu_data cpus) sched_ptr).
  {
    eapply field_at_Tarray.
    { cbv; auto. }
    { reflexivity. }
    { lia. }
    { apply JMeq_refl. }
  }
  rewrite Hfield_array.
  eapply derives_trans.
  {
    apply array_at_ramif
      with (t0 := SchedClight.t_sched_cpu)
           (n := 256)
           (a := noattr)
           (v0 := SchedClight.sched_cpu_data cpu).
	    - reflexivity.
    - pose proof (proj1 (Nat2Z.inj_lt cpu_id sched_max_cpus) Hlt) as HltZ.
      change (Z.of_nat sched_max_cpus) with 256 in HltZ.
      split.
      + apply Nat2Z.is_nonneg.
      + exact HltZ.
    - rewrite Z.sub_0_r.
      rewrite Znth_map.
      rewrite (Znth_of_nat_nth_error cpus cpu_id cpu Hlookup).
      + apply JMeq_refl.
      + pose proof (proj1 (Nat2Z.inj_lt cpu_id sched_max_cpus) Hlt) as HltZ.
        change (Z.of_nat sched_max_cpus) with 256 in HltZ.
        rewrite Zlength_correct, Hlen.
        lia.
  }
  apply sepcon_derives.
  - unfold SchedClight.sched_cpu_data_rep,
      SchedClight.sched_cpu_array_element_ptr.
    rewrite field_at_data_at.
    apply derives_refl.
  - apply allp_left with (SchedClight.sched_cpu_data cpu).
    apply allp_left with (SchedClight.sched_cpu_data cpu).
    rewrite prop_imp by apply JMeq_refl.
    apply wand_derives.
    + unfold SchedClight.sched_cpu_data_rep,
        SchedClight.sched_cpu_array_element_ptr.
      rewrite field_at_data_at.
      apply derives_refl.
    + rewrite Z.sub_0_r.
      erewrite upd_Znth_triv.
      * rewrite <- Hfield_array.
        apply derives_refl.
      * rewrite Zlength_map, Zlength_correct, Hlen.
        pose proof (proj1 (Nat2Z.inj_lt cpu_id sched_max_cpus) Hlt) as HltZ.
        change (Z.of_nat sched_max_cpus) with 256 in HltZ.
        split.
        -- apply Nat2Z.is_nonneg.
        -- exact HltZ.
      * rewrite Znth_map.
        -- rewrite (Znth_of_nat_nth_error cpus cpu_id cpu Hlookup).
           reflexivity.
        -- rewrite Zlength_correct, Hlen.
           pose proof (proj1 (Nat2Z.inj_lt cpu_id sched_max_cpus) Hlt) as HltZ.
           change (Z.of_nat sched_max_cpus) with 256 in HltZ.
           split.
           ++ apply Nat2Z.is_nonneg.
           ++ exact HltZ.
Qed.

Lemma body_pacha_sched_no_decision :
  semax_body
    SchedClight.Vprog
    SchedClight.Gprog
    C.f_pacha_sched_no_decision
    SchedClight.pacha_sched_no_decision_spec.
Proof.
  start_function.
  forward.
  forward.
  forward.
  forward.
  forward.
Qed.

Lemma body_map_eevdf_rc :
  semax_body
    SchedClight.Vprog
    SchedClight.Gprog
    C.f_map_eevdf_rc
    SchedClight.map_eevdf_rc_spec.
Proof.
  start_function.
  destruct rc; simpl; forward_if (PROP (False) LOCAL () SEP ()).
  all: try contradiction.
  all: forward.
Qed.

Lemma body_idle_decision :
  semax_body
    SchedClight.Vprog
    SchedClight.Gprog
    C.f_idle_decision
    SchedClight.idle_decision_spec.
Proof.
  start_function.
  forward.
  forward.
  forward.
  forward.
  forward.
Qed.

Lemma body_run_thread_decision :
  semax_body
    SchedClight.Vprog
    SchedClight.Gprog
    C.f_run_thread_decision
    SchedClight.run_thread_decision_spec.
Proof.
  start_function.
  forward.
  forward.
  forward.
  forward.
  forward.
  forward.
  forward.
Qed.

Lemma body_valid_cpu :
  semax_body
    SchedClight.Vprog
    SchedClight.Gprog
    C.f_valid_cpu
    SchedClight.valid_cpu_spec.
Proof.
  start_function.
  forward.
  forward.
  entailer!.
  unfold SchedClight.v_bool, SchedClight.v_int, SchedClight.v_size.
  unfold sem_cast_i2i, both_long, sem_cast_pointer.
  simpl.
  f_equal.
  f_equal.
  unfold Int64.ltu.
  assert (Hcpu_id_range :
    0 <= Z.of_nat cpu_id <= Int64.max_unsigned)
    by (change Int64.max_unsigned with 18446744073709551615;
        unfold c_size_t_value, c_size_t_max in *; lia).
  assert (Hcpu_count_range :
    0 <= Z.of_nat cpu_count <= Int64.max_unsigned)
    by (change Int64.max_unsigned with 18446744073709551615;
        unfold c_size_t_value, c_size_t_max in *; lia).
  rewrite (Int64.unsigned_repr (Z.of_nat cpu_id)) by exact Hcpu_id_range.
  rewrite (Int64.unsigned_repr (Z.of_nat cpu_count)) by exact Hcpu_count_range.
  destruct (zlt (Z.of_nat cpu_id) (Z.of_nat cpu_count)) as [Hlt | Hnlt].
  - destruct (cpu_id <? cpu_count)%nat eqn:Hcmp.
    + reflexivity.
    + apply Nat.ltb_ge in Hcmp; lia.
  - destruct (cpu_id <? cpu_count)%nat eqn:Hcmp.
    + apply Nat.ltb_lt in Hcmp; lia.
	    + reflexivity.
Qed.

Lemma body_sched_cpu_const_ptr :
  semax_body
    SchedClight.Vprog
    SchedClight.Gprog
    C.f_sched_cpu_const_ptr
    SchedClight.sched_cpu_const_ptr_spec.
Proof.
  start_function.
  rename H into Hcpu_id_size.
  rename H0 into Hcpus_len.
  rename H1 into Hcpu_lookup.
  assert (Hcpu_id_lt_cpus : (cpu_id < length cpus)%nat).
  {
    apply nth_error_Some.
    rewrite Hcpu_lookup.
    discriminate.
  }
  assert (Hcpu_id_lt : (cpu_id < sched_max_cpus)%nat)
    by (rewrite <- Hcpus_len; exact Hcpu_id_lt_cpus).
  assert_PROP (field_compatible SchedClight.t_sched_state
    [StructField C._cpus] sched_ptr) as Hcpus_compat by entailer!.
  assert (Harray_compat :
    field_compatible
      (nested_field_type SchedClight.t_sched_state
        [StructField C._cpus])
      [ArraySubsc (Z.of_nat cpu_id)]
      (field_address SchedClight.t_sched_state
        [StructField C._cpus] sched_ptr)).
  {
    rewrite field_compatible_field_address by exact Hcpus_compat.
    eapply field_compatible_cons_Tarray
      with (t0 := SchedClight.t_sched_cpu)
           (n := 256)
           (a := noattr)
           (gfs := @nil gfield).
    - reflexivity.
    - apply field_compatible_nested_field.
      exact Hcpus_compat.
    - change sched_max_cpus with 256%nat in Hcpu_id_lt.
      lia.
  }
  forward.
  sep_apply (sched_cpu_array_element_ramif
    Tsh cpus cpu sched_ptr cpu_id Hcpus_len Hcpu_lookup Hcpu_id_lt).
  entailer!.
  unfold SchedClight.sched_cpu_array_element_ptr.
  replace (offset_val 18464 sched_ptr)
    with (field_address SchedClight.t_sched_state
      [StructField C._cpus] sched_ptr)
    by (rewrite field_address_offset by exact Hcpus_compat;
        change (nested_field_offset SchedClight.t_sched_state
          [StructField C._cpus]) with 18464;
        reflexivity).
  rewrite sched_cpu_array_element_sem_add by eassumption.
  reflexivity.
Qed.

Lemma body_sched_cpu_ptr :
  semax_body
    SchedClight.Vprog
    SchedClight.Gprog
    C.f_sched_cpu_ptr
    SchedClight.sched_cpu_ptr_spec.
Proof.
  start_function.
  rename H into Hcpu_id_size.
  rename H0 into Hcpus_len.
  rename H1 into Hcpu_lookup.
  assert (Hcpu_id_lt_cpus : (cpu_id < length cpus)%nat).
  {
    apply nth_error_Some.
    rewrite Hcpu_lookup.
    discriminate.
  }
  assert (Hcpu_id_lt : (cpu_id < sched_max_cpus)%nat)
    by (rewrite <- Hcpus_len; exact Hcpu_id_lt_cpus).
  assert_PROP (field_compatible SchedClight.t_sched_state
    [StructField C._cpus] sched_ptr) as Hcpus_compat by entailer!.
  assert (Harray_compat :
    field_compatible
      (nested_field_type SchedClight.t_sched_state
        [StructField C._cpus])
      [ArraySubsc (Z.of_nat cpu_id)]
      (field_address SchedClight.t_sched_state
        [StructField C._cpus] sched_ptr)).
  {
    rewrite field_compatible_field_address by exact Hcpus_compat.
    eapply field_compatible_cons_Tarray
      with (t0 := SchedClight.t_sched_cpu)
           (n := 256)
           (a := noattr)
           (gfs := @nil gfield).
    - reflexivity.
    - apply field_compatible_nested_field.
      exact Hcpus_compat.
    - change sched_max_cpus with 256%nat in Hcpu_id_lt.
      lia.
  }
  forward.
  sep_apply (sched_cpu_array_element_ramif
    Tsh cpus cpu sched_ptr cpu_id Hcpus_len Hcpu_lookup Hcpu_id_lt).
  entailer!.
  unfold SchedClight.sched_cpu_array_element_ptr.
  replace (offset_val 18464 sched_ptr)
    with (field_address SchedClight.t_sched_state
      [StructField C._cpus] sched_ptr)
    by (rewrite field_address_offset by exact Hcpus_compat;
        change (nested_field_offset SchedClight.t_sched_state
          [StructField C._cpus]) with 18464;
        reflexivity).
  rewrite sched_cpu_array_element_sem_add by eassumption.
  reflexivity.
Qed.

Lemma body_sched_cpu_has_current :
  semax_body
    SchedClight.Vprog
    SchedClight.Gprog
    C.f_sched_cpu_has_current
    SchedClight.sched_cpu_has_current_spec.
Proof.
  start_function.
  forward.
  forward.
Qed.

Lemma body_cpu_has_current :
  semax_body
    SchedClight.Vprog
    SchedClight.Gprog
    C.f_cpu_has_current
    SchedClight.cpu_has_current_spec.
Proof.
  start_function.
  rename H into Hcpu_id_size.
  rename H0 into Hcpu_count_size.
  rename H1 into Hcpus_len.
  rename H2 into Hcpu_lookup.
  rename H3 into Hcpu_shape.
  assert (Hcpu_id_lt_cpus : (cpu_id < length cpus)%nat).
  {
    apply nth_error_Some.
    rewrite Hcpu_lookup.
    discriminate.
  }
  assert (Hcpu_id_lt_max : (cpu_id < sched_max_cpus)%nat)
    by (rewrite <- Hcpus_len; exact Hcpu_id_lt_cpus).
  forward.
  forward_call (sched_ptr, cpu_id, cpu_count).
  destruct (cpu_id <? cpu_count)%nat eqn:Hvalid.
  - forward_if (
      PROP ()
      LOCAL (
        temp C._result (SchedClight.v_bool (sc_has_current cpu));
        temp C._t'2 (SchedClight.v_bool (sc_has_current cpu));
        temp C._t'1
          (SchedClight.sched_cpu_array_element_ptr sched_ptr cpu_id);
        temp C._t'3 (SchedClight.v_bool true);
        temp C._sched sched_ptr;
        temp C._cpu_id (SchedClight.v_size cpu_id)
      )
      SEP (
        field_at Tsh SchedClight.t_sched_state
          [StructField C._cpu_count]
          (SchedClight.v_size cpu_count) sched_ptr;
        field_at Tsh SchedClight.t_sched_state
          [StructField C._cpus]
          (map SchedClight.sched_cpu_data cpus) sched_ptr
      )).
    + forward_call (sched_ptr, cpu_id, cpus, cpu);
        try solve [entailer!].
      forward_call
        (SchedClight.sched_cpu_array_element_ptr sched_ptr cpu_id, cpu);
        try solve [entailer!].
      forward.
      sep_apply (modus_ponens_wand'
        (SchedClight.sched_cpu_data_rep Tsh cpu
          (SchedClight.sched_cpu_array_element_ptr sched_ptr cpu_id))).
      entailer!.
      change (nested_field_type SchedClight.t_sched_state
        [StructField C._cpus])
        with (Tarray SchedClight.t_sched_cpu 256 noattr).
      rewrite <- sched_cpu_array_field_at_data_at.
      entailer!.
    + exfalso.
      match goal with
      | Hbad : Int.repr 1 = Int.zero |- _ =>
          change (Int.repr 1) with Int.one in Hbad;
          exact (Int.one_not_zero Hbad)
      end.
	    + forward.
  - forward_if (
      PROP ()
      LOCAL (
        temp C._result (SchedClight.v_bool false);
        temp C._t'3 (SchedClight.v_bool false);
        temp C._sched sched_ptr;
        temp C._cpu_id (SchedClight.v_size cpu_id)
      )
      SEP (
        field_at Tsh SchedClight.t_sched_state
          [StructField C._cpu_count]
          (SchedClight.v_size cpu_count) sched_ptr;
        field_at Tsh SchedClight.t_sched_state
          [StructField C._cpus]
          (map SchedClight.sched_cpu_data cpus) sched_ptr
      )).
    + exfalso.
      match goal with
      | Hbad : Int.zero <> Int.zero |- _ => exact (Hbad eq_refl)
      | Hbad : Int.repr 0 <> Int.zero |- _ =>
          change (Int.repr 0) with Int.zero in Hbad;
          exact (Hbad eq_refl)
      end.
    + forward.
      entailer!.
	    + forward.
Qed.

Lemma body_clear_cpu_current :
  semax_body
    SchedClight.Vprog
    SchedClight.Gprog
    C.f_clear_cpu_current
    SchedClight.clear_cpu_current_spec.
Proof.
  start_function.
  forward.
  forward.
  forward.
Qed.

Lemma body_set_cpu_current :
  semax_body
    SchedClight.Vprog
    SchedClight.Gprog
    C.f_set_cpu_current
    SchedClight.set_cpu_current_spec.
Proof.
  start_function.
  forward.
  forward.
  forward.
Qed.

Lemma body_pacha_sched_add_thread :
  semax_body
    SchedClight.Vprog
    SchedClight.Gprog
    C.f_pacha_sched_add_thread
    SchedClight.pacha_sched_add_thread_spec.
Proof.
  start_function.
  forward_call decision_out.
  assert_PROP (field_compatible SchedClight.t_sched_state
    [StructField C._runqueue] sched_ptr) as Hrunqueue_compat
    by entailer!.
  forward_call
    (ss_runqueue sched,
     field_address SchedClight.t_sched_state
       [StructField C._runqueue] sched_ptr,
     thread_id, generation, weight, slice_ns, scratch).
  - simpl.
    entailer!.
    rewrite field_address_offset by exact Hrunqueue_compat.
    change (nested_field_offset SchedClight.t_sched_state
      [StructField C._runqueue]) with 0.
    rewrite isptr_offset_val_zero
      by (eapply field_compatible_isptr; exact Hrunqueue_compat).
    reflexivity.
  - unfold eevdf_add_vst_pre, sched_add_thread_vst_pre,
      sched_state_c_shape in *.
    tauto.
  - forward_if.
    + forward_call
        (eevdf_result_rc
          (eevdf_add (ss_runqueue sched) thread_id generation weight slice_ns)).
      forward.
      assert (Hadd_rc_not_ok :
        eevdf_result_rc
          (eevdf_add (ss_runqueue sched) thread_id generation weight slice_ns)
        <> EevdfOk).
      {
        intro Hrc_ok.
        apply H1.
        rewrite Hrc_ok.
        reflexivity.
      }
      rewrite (eevdf_add_non_ok_runqueue
        (ss_runqueue sched) thread_id generation weight slice_ns
        Hadd_rc_not_ok).
      unfold sched_add_thread, sched_apply_eevdf_result,
        sched_add_thread_vst_post.
      destruct (eevdf_result_rc
        (eevdf_add (ss_runqueue sched) thread_id generation weight slice_ns))
        eqn:Hadd_rc; try contradiction; simpl; entailer!.
      all: eexists; split;
        [ unfold sched_add_thread, sched_apply_eevdf_result;
          rewrite Hadd_rc; reflexivity
        | split; [reflexivity | split; [reflexivity |]];
          apply sched_fail_map_eevdf_rc_c_shape ].
    + assert (Hadd_rc_ok :
        eevdf_result_rc
          (eevdf_add (ss_runqueue sched) thread_id generation weight slice_ns) =
        EevdfOk).
      {
        apply eevdf_rc_c_repr_zero_ok.
        exact H1.
      }
      forward_call
        (eevdf_result_rq
          (eevdf_add (ss_runqueue sched) thread_id generation weight slice_ns),
         ss_runqueue sched,
         scratch,
         field_address SchedClight.t_sched_state
           [StructField C._runqueue] sched_ptr).
      simpl.
      entailer!.
      rewrite field_address_offset by exact Hrunqueue_compat.
      change (nested_field_offset SchedClight.t_sched_state
        [StructField C._runqueue]) with 0.
      rewrite isptr_offset_val_zero
        by (eapply field_compatible_isptr; exact Hrunqueue_compat).
      reflexivity.
      forward.
      unfold sched_add_thread, sched_apply_eevdf_result,
        sched_add_thread_vst_post.
      rewrite Hadd_rc_ok.
      simpl.
      entailer!.
      eexists; split.
      * unfold sched_add_thread, sched_apply_eevdf_result.
        rewrite Hadd_rc_ok.
        reflexivity.
      * split; [reflexivity | split; [reflexivity |]].
        apply sched_ok_no_decision_c_shape.
Qed.

Lemma body_pacha_sched_wake_thread :
  semax_body
    SchedClight.Vprog
    SchedClight.Gprog
    C.f_pacha_sched_wake_thread
    SchedClight.pacha_sched_wake_thread_spec.
Proof.
  start_function.
  forward_call decision_out.
  assert_PROP (field_compatible SchedClight.t_sched_state
    [StructField C._runqueue] sched_ptr) as Hrunqueue_compat
    by entailer!.
  forward_call
    (ss_runqueue sched,
     field_address SchedClight.t_sched_state
       [StructField C._runqueue] sched_ptr,
     thread_id, scratch).
  - simpl.
    entailer!.
    rewrite field_address_offset by exact Hrunqueue_compat.
    change (nested_field_offset SchedClight.t_sched_state
      [StructField C._runqueue]) with 0.
    rewrite isptr_offset_val_zero
      by (eapply field_compatible_isptr; exact Hrunqueue_compat).
    reflexivity.
  - unfold eevdf_unary_thread_vst_pre, sched_unary_thread_vst_pre,
      sched_state_c_shape in *.
    tauto.
  - forward_if.
    + forward_call
        (eevdf_result_rc (eevdf_wake (ss_runqueue sched) thread_id)).
      forward.
      assert (Hwake_rc_not_ok :
        eevdf_result_rc (eevdf_wake (ss_runqueue sched) thread_id) <>
        EevdfOk).
      {
        intro Hrc_ok.
        apply H1.
        rewrite Hrc_ok.
        reflexivity.
      }
      rewrite (eevdf_wake_non_ok_runqueue
        (ss_runqueue sched) thread_id Hwake_rc_not_ok).
      unfold sched_wake_thread, sched_apply_eevdf_result,
        sched_wake_thread_vst_post.
      destruct (eevdf_result_rc (eevdf_wake (ss_runqueue sched) thread_id))
        eqn:Hwake_rc; try contradiction; simpl; entailer!.
      all: eexists; split;
        [ unfold sched_wake_thread, sched_apply_eevdf_result;
          rewrite Hwake_rc; reflexivity
        | split; [reflexivity | split; [reflexivity |]];
          apply sched_fail_map_eevdf_rc_c_shape ].
    + assert (Hwake_rc_ok :
        eevdf_result_rc (eevdf_wake (ss_runqueue sched) thread_id) =
        EevdfOk).
      {
        apply eevdf_rc_c_repr_zero_ok.
        exact H1.
      }
      forward_call
        (eevdf_result_rq (eevdf_wake (ss_runqueue sched) thread_id),
         ss_runqueue sched,
         scratch,
         field_address SchedClight.t_sched_state
           [StructField C._runqueue] sched_ptr).
      simpl.
      entailer!.
      rewrite field_address_offset by exact Hrunqueue_compat.
      change (nested_field_offset SchedClight.t_sched_state
        [StructField C._runqueue]) with 0.
      rewrite isptr_offset_val_zero
        by (eapply field_compatible_isptr; exact Hrunqueue_compat).
      reflexivity.
      forward.
      unfold sched_wake_thread, sched_apply_eevdf_result,
        sched_wake_thread_vst_post.
      rewrite Hwake_rc_ok.
      simpl.
      entailer!.
      eexists; split.
      * unfold sched_wake_thread, sched_apply_eevdf_result.
        rewrite Hwake_rc_ok.
        reflexivity.
      * split; [reflexivity | split; [reflexivity |]].
        apply sched_ok_no_decision_c_shape.
Qed.

Lemma body_pacha_sched_block_thread :
  semax_body
    SchedClight.Vprog
    SchedClight.Gprog
    C.f_pacha_sched_block_thread
    SchedClight.pacha_sched_block_thread_spec.
Proof.
  start_function.
  forward_call decision_out.
  assert_PROP (field_compatible SchedClight.t_sched_state
    [StructField C._runqueue] sched_ptr) as Hrunqueue_compat
    by entailer!.
  forward_call
    (ss_runqueue sched,
     field_address SchedClight.t_sched_state
       [StructField C._runqueue] sched_ptr,
     thread_id, scratch).
  - simpl.
    entailer!.
    rewrite field_address_offset by exact Hrunqueue_compat.
    change (nested_field_offset SchedClight.t_sched_state
      [StructField C._runqueue]) with 0.
    rewrite isptr_offset_val_zero
      by (eapply field_compatible_isptr; exact Hrunqueue_compat).
    reflexivity.
  - unfold eevdf_unary_thread_vst_pre, sched_unary_thread_vst_pre,
      sched_state_c_shape in *.
    tauto.
  - forward_if.
    + forward_call
        (eevdf_result_rc (eevdf_block (ss_runqueue sched) thread_id)).
      forward.
      assert (Hblock_rc_not_ok :
        eevdf_result_rc (eevdf_block (ss_runqueue sched) thread_id) <>
        EevdfOk).
      {
        intro Hrc_ok.
        apply H1.
        rewrite Hrc_ok.
        reflexivity.
      }
      rewrite (eevdf_block_non_ok_runqueue
        (ss_runqueue sched) thread_id Hblock_rc_not_ok).
      unfold sched_block_thread, sched_block_thread_vst_post.
      destruct (eevdf_block (ss_runqueue sched) thread_id) as [block_rc block_rq]
        eqn:Hblock.
      simpl in Hblock_rc_not_ok.
      destruct block_rc eqn:Hblock_rc; try contradiction; simpl; entailer!.
      all: match goal with
      | Hblock' : eevdf_block _ _ =
          {| eevdf_result_rc := ?rc; eevdf_result_rq := ?rq |} |- _ =>
          exists (sched_fail (map_eevdf_rc rc));
          split;
          [ unfold sched_block_thread; rewrite Hblock'; reflexivity
          | split; [reflexivity | split; [reflexivity |]];
            apply sched_fail_map_eevdf_rc_c_shape ]
      end.
    + assert (Hblock_rc_ok :
        eevdf_result_rc (eevdf_block (ss_runqueue sched) thread_id) =
        EevdfOk).
      {
        apply eevdf_rc_c_repr_zero_ok.
        exact H1.
      }
      assert (Hblock_rq_shape :
        eevdf_runqueue_c_shape
          (eevdf_result_rq (eevdf_block (ss_runqueue sched) thread_id))).
      {
        unfold eevdf_block_vst_post, eevdf_result_c_shape in H2.
        tauto.
      }
      forward_call
        (eevdf_result_rq (eevdf_block (ss_runqueue sched) thread_id),
         ss_runqueue sched,
         scratch,
         field_address SchedClight.t_sched_state
           [StructField C._runqueue] sched_ptr).
      simpl.
      entailer!.
      rewrite field_address_offset by exact Hrunqueue_compat.
      change (nested_field_offset SchedClight.t_sched_state
        [StructField C._runqueue]) with 0.
      rewrite isptr_offset_val_zero
        by (eapply field_compatible_isptr; exact Hrunqueue_compat).
      reflexivity.
      forward_call
        (with_runqueue sched
          (eevdf_result_rq (eevdf_block (ss_runqueue sched) thread_id)),
         sched_ptr,
         thread_id).
      {
        unfold sched_unary_thread_vst_pre, sched_state_c_shape,
          with_runqueue in *.
        simpl in *.
        tauto.
      }
      forward.
      unfold sched_block_thread, sched_block_thread_vst_post.
      destruct (eevdf_block (ss_runqueue sched) thread_id) eqn:Hblock.
      simpl in Hblock_rc_ok.
      rewrite Hblock_rc_ok.
      simpl.
      entailer!.
      eexists; split.
      * apply sched_block_thread_success_spec.
        exact Hblock.
      * split; [reflexivity | split; [reflexivity |]].
        apply sched_ok_no_decision_c_shape.
Qed.

Lemma body_pacha_sched_exit_thread :
  semax_body
    SchedClight.Vprog
    SchedClight.Gprog
    C.f_pacha_sched_exit_thread
    SchedClight.pacha_sched_exit_thread_spec.
Proof.
  start_function.
  forward_call decision_out.
  assert_PROP (field_compatible SchedClight.t_sched_state
    [StructField C._runqueue] sched_ptr) as Hrunqueue_compat
    by entailer!.
  forward_call
    (ss_runqueue sched,
     field_address SchedClight.t_sched_state
       [StructField C._runqueue] sched_ptr,
     thread_id, scratch).
  - simpl.
    entailer!.
    rewrite field_address_offset by exact Hrunqueue_compat.
    change (nested_field_offset SchedClight.t_sched_state
      [StructField C._runqueue]) with 0.
    rewrite isptr_offset_val_zero
      by (eapply field_compatible_isptr; exact Hrunqueue_compat).
    reflexivity.
  - unfold eevdf_unary_thread_vst_pre, sched_unary_thread_vst_pre,
      sched_state_c_shape in *.
    tauto.
  - forward_if.
    + forward_call
        (eevdf_result_rc (eevdf_exit (ss_runqueue sched) thread_id)).
      forward.
      assert (Hexit_rc_not_ok :
        eevdf_result_rc (eevdf_exit (ss_runqueue sched) thread_id) <>
        EevdfOk).
      {
        intro Hrc_ok.
        apply H1.
        rewrite Hrc_ok.
        reflexivity.
      }
      rewrite (eevdf_exit_non_ok_runqueue
        (ss_runqueue sched) thread_id Hexit_rc_not_ok).
      unfold sched_exit_thread, sched_exit_thread_vst_post.
      destruct (eevdf_exit (ss_runqueue sched) thread_id) as [exit_rc exit_rq]
        eqn:Hexit.
      simpl in Hexit_rc_not_ok.
      destruct exit_rc eqn:Hexit_rc; try contradiction; simpl; entailer!.
      all: match goal with
      | Hexit' : eevdf_exit _ _ =
          {| eevdf_result_rc := ?rc; eevdf_result_rq := ?rq |} |- _ =>
          exists (sched_fail (map_eevdf_rc rc));
          split;
          [ unfold sched_exit_thread; rewrite Hexit'; reflexivity
          | split; [reflexivity | split; [reflexivity |]];
            apply sched_fail_map_eevdf_rc_c_shape ]
      end.
    + assert (Hexit_rc_ok :
        eevdf_result_rc (eevdf_exit (ss_runqueue sched) thread_id) =
        EevdfOk).
      {
        apply eevdf_rc_c_repr_zero_ok.
        exact H1.
      }
      assert (Hexit_rq_shape :
        eevdf_runqueue_c_shape
          (eevdf_result_rq (eevdf_exit (ss_runqueue sched) thread_id))).
      {
        unfold eevdf_exit_vst_post, eevdf_result_c_shape in H2.
        tauto.
      }
      forward_call
        (eevdf_result_rq (eevdf_exit (ss_runqueue sched) thread_id),
         ss_runqueue sched,
         scratch,
         field_address SchedClight.t_sched_state
           [StructField C._runqueue] sched_ptr).
      simpl.
      entailer!.
      rewrite field_address_offset by exact Hrunqueue_compat.
      change (nested_field_offset SchedClight.t_sched_state
        [StructField C._runqueue]) with 0.
      rewrite isptr_offset_val_zero
        by (eapply field_compatible_isptr; exact Hrunqueue_compat).
      reflexivity.
      forward_call
        (with_runqueue sched
          (eevdf_result_rq (eevdf_exit (ss_runqueue sched) thread_id)),
         sched_ptr,
         thread_id).
      {
        unfold sched_unary_thread_vst_pre, sched_state_c_shape,
          with_runqueue in *.
        simpl in *.
        tauto.
      }
      forward.
      unfold sched_exit_thread, sched_exit_thread_vst_post.
      destruct (eevdf_exit (ss_runqueue sched) thread_id) eqn:Hexit.
      simpl in Hexit_rc_ok.
      rewrite Hexit_rc_ok.
      simpl.
      entailer!.
      eexists; split.
      * apply sched_exit_thread_success_spec.
        exact Hexit.
      * split; [reflexivity | split; [reflexivity |]].
        apply sched_ok_no_decision_c_shape.
Qed.

Lemma body_pacha_sched_on_timer :
  semax_body
    SchedClight.Vprog
    SchedClight.Gprog
    C.f_pacha_sched_on_timer
    SchedClight.pacha_sched_on_timer_spec.
Proof.
  start_function.
  destruct H as [Hcpu_pre Hruntime_shape].
  destruct Hcpu_pre as [[Hrunqueue_shape Hstate_tail] Hcpu_id_size].
  rename H0 into Hrunqueue_compat.
  forward_call decision_out.
  forward.
  destruct (cpu_id <? ss_cpu_count sched)%nat eqn:Hvalid_cmp.
  - assert (Hvalid : valid_cpu sched cpu_id = true)
      by (unfold valid_cpu; exact Hvalid_cmp).
    forward_if.
    + forward_call
        (sched, sched_ptr, cpu_id, runtime_ns, scratch, scratch_before).
      {
        unfold sched_timer_vst_pre, sched_cpu_vst_pre.
        split.
        - split.
          + unfold sched_state_c_shape.
            split; [exact Hrunqueue_shape | exact Hstate_tail].
          + exact Hcpu_id_size.
        - exact Hruntime_shape.
      }
      forward.
      rewrite (sched_on_timer_valid_decision_no_decision
        sched cpu_id runtime_ns Hvalid).
      cancel.
    + exfalso.
      destruct Hstate_tail as [_ [_ [Hcpu_count_size _]]].
      match goal with
      | Hbad : Int64.ltu _ _ = false |- _ =>
          rewrite (int64_ltu_size_nat_true cpu_id (ss_cpu_count sched)
            Hcpu_id_size Hcpu_count_size Hvalid_cmp) in Hbad;
          discriminate
      end.
  - assert (Hvalid : valid_cpu sched cpu_id = false)
      by (unfold valid_cpu; exact Hvalid_cmp).
    forward_if.
    + exfalso.
      destruct Hstate_tail as [_ [_ [Hcpu_count_size _]]].
      match goal with
      | Hbad : Int64.ltu _ _ = true |- _ =>
          rewrite (int64_ltu_size_nat_false cpu_id (ss_cpu_count sched)
            Hcpu_id_size Hcpu_count_size Hvalid_cmp) in Hbad;
          discriminate
      end.
    + forward.
      unfold sched_on_timer, sched_on_timer_vst_post,
        SchedClight.sched_on_timer_scratch_rq.
      rewrite Hvalid.
      simpl.
      entailer!.
      eexists; split.
      * apply sched_on_timer_invalid_cpu_spec.
        exact Hvalid.
      * split; [reflexivity | split; [reflexivity |]].
        apply sched_fail_c_shape.
Qed.

Lemma body_pacha_sched_finish_current :
  semax_body
    SchedClight.Vprog
    SchedClight.Gprog
    C.f_pacha_sched_finish_current
    SchedClight.pacha_sched_finish_current_spec.
Proof.
  start_function.
  destruct H as [[Hrunqueue_shape Hstate_tail] Hcpu_id_size].
  rename H0 into Hrunqueue_compat.
  forward_call decision_out.
  forward.
  destruct (cpu_id <? ss_cpu_count sched)%nat eqn:Hvalid_cmp.
  - assert (Hvalid : valid_cpu sched cpu_id = true)
      by (unfold valid_cpu; exact Hvalid_cmp).
    forward_if.
    + forward_call
        (sched, sched_ptr, cpu_id, scratch, scratch_before).
      {
        unfold sched_cpu_vst_pre.
        split.
        - unfold sched_state_c_shape.
          split; [exact Hrunqueue_shape | exact Hstate_tail].
        - exact Hcpu_id_size.
      }
      forward.
      rewrite (sched_finish_current_valid_decision_no_decision
        sched cpu_id Hvalid).
      cancel.
    + exfalso.
      destruct Hstate_tail as [_ [_ [Hcpu_count_size _]]].
      match goal with
      | Hbad : Int64.ltu _ _ = false |- _ =>
          rewrite (int64_ltu_size_nat_true cpu_id (ss_cpu_count sched)
            Hcpu_id_size Hcpu_count_size Hvalid_cmp) in Hbad;
          discriminate
      end.
  - assert (Hvalid : valid_cpu sched cpu_id = false)
      by (unfold valid_cpu; exact Hvalid_cmp).
    forward_if.
    + exfalso.
      destruct Hstate_tail as [_ [_ [Hcpu_count_size _]]].
      match goal with
      | Hbad : Int64.ltu _ _ = true |- _ =>
          rewrite (int64_ltu_size_nat_false cpu_id (ss_cpu_count sched)
            Hcpu_id_size Hcpu_count_size Hvalid_cmp) in Hbad;
          discriminate
      end.
    + forward.
      unfold sched_finish_current, sched_finish_current_vst_post,
        SchedClight.sched_finish_current_scratch_rq.
      rewrite Hvalid.
      simpl.
      entailer!.
      eexists; split.
      * apply sched_finish_current_invalid_cpu_spec.
        exact Hvalid.
      * split; [reflexivity | split; [reflexivity |]].
        apply sched_fail_c_shape.
Qed.
