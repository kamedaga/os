From Stdlib Require Import Lists.List ZArith.ZArith.
From VST Require Import floyd.proofauto.
From Pacha.Scheduling Require Import
  ProtocolModel
  EevdfModel
  EevdfTransitions.
From Pacha.Scheduling.Proof Require Import
  SchedRuntimeVstSpec.
From Pacha.Scheduling.Clight Require Import PachaEevdfClight.

Import ListNotations.
Open Scope Z_scope.
Open Scope logic.

Module C := PachaEevdfClight.

Module EevdfClight.
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

  Definition v_i64
      (value : Z)
    : val :=
    Vlong (Int64.repr value).

  Definition v_int
      (value : Z)
    : val :=
    Vint (Int.repr value).

  Definition v_size
      (value : nat)
    : val :=
    Vlong (Int64.repr (Z.of_nat value)).

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

  Definition eevdf_entity_data_rep
      (sh : share)
      (entity : eevdf_entity)
      (ptr : val)
    : mpred :=
    data_at sh t_eevdf_entity (eevdf_entity_data entity) ptr.

  Definition eevdf_runqueue_data
      (rq : eevdf_runqueue)
    : reptype t_eevdf_runqueue :=
    (map eevdf_entity_data (er_entities rq),
     (v_size (er_entity_count rq),
      (v_size (er_runnable_count rq),
       (v_i64 (er_virtual_time rq),
        v_i64 (er_min_vruntime rq))))).

  Definition eevdf_runqueue_data_rep
      (sh : share)
      (rq : eevdf_runqueue)
      (ptr : val)
    : mpred :=
    data_at sh t_eevdf_runqueue (eevdf_runqueue_data rq) ptr.

  Definition eevdf_entity_array_element_ptr
      (rq_ptr : val)
      (index : nat)
    : val :=
    field_address t_eevdf_runqueue
      [ArraySubsc (Z.of_nat index); StructField C._entities]
      rq_ptr.

  Definition eevdf_entity_array_element_rep
      (sh : share)
      (entity : eevdf_entity)
      (rq_ptr : val)
      (index : nat)
    : mpred :=
    field_at sh t_eevdf_runqueue
      [ArraySubsc (Z.of_nat index); StructField C._entities]
      (eevdf_entity_data entity) rq_ptr.

  Definition eevdf_entity_array_element_field_rep
      (sh : share)
      (entity : eevdf_entity)
      (rq_ptr : val)
      (index : nat)
    : mpred :=
    field_at sh t_eevdf_runqueue
      [StructField C._thread_id;
       ArraySubsc (Z.of_nat index);
       StructField C._entities]
      (v_i64 (ee_thread_id entity)) rq_ptr *
    field_at sh t_eevdf_runqueue
      [StructField C._generation;
       ArraySubsc (Z.of_nat index);
       StructField C._entities]
      (v_i64 (ee_generation entity)) rq_ptr *
    field_at sh t_eevdf_runqueue
      [StructField C._weight;
       ArraySubsc (Z.of_nat index);
       StructField C._entities]
      (v_i64 (ee_weight entity)) rq_ptr *
    field_at sh t_eevdf_runqueue
      [StructField C._slice_ns;
       ArraySubsc (Z.of_nat index);
       StructField C._entities]
      (v_i64 (ee_slice_ns entity)) rq_ptr *
    field_at sh t_eevdf_runqueue
      [StructField C._service_ns;
       ArraySubsc (Z.of_nat index);
       StructField C._entities]
      (v_i64 (ee_service_ns entity)) rq_ptr *
    field_at sh t_eevdf_runqueue
      [StructField C._vruntime;
       ArraySubsc (Z.of_nat index);
       StructField C._entities]
      (v_i64 (ee_vruntime entity)) rq_ptr *
    field_at sh t_eevdf_runqueue
      [StructField C._eligible_time;
       ArraySubsc (Z.of_nat index);
       StructField C._entities]
      (v_i64 (ee_eligible_time entity)) rq_ptr *
    field_at sh t_eevdf_runqueue
      [StructField C._deadline;
       ArraySubsc (Z.of_nat index);
       StructField C._entities]
      (v_i64 (ee_deadline entity)) rq_ptr *
    field_at sh t_eevdf_runqueue
      [StructField C._state;
       ArraySubsc (Z.of_nat index);
       StructField C._entities]
      (eevdf_state_c_val (ee_state entity)) rq_ptr.

  Definition valid_positive_spec : ident * funspec :=
    DECLARE C._valid_positive
    WITH value : Z
    PRE [ tlong ]
      PROP (c_int64_value value)
      PARAMS (v_i64 value)
      SEP ()
    POST [ tint ]
      PROP ()
      RETURN (v_bool (0 <? value))
      SEP ().

  Definition i64_less_spec : ident * funspec :=
    DECLARE C._i64_less
    WITH lhs : Z, rhs : Z
    PRE [ tlong, tlong ]
      PROP (
        c_int64_value lhs;
        c_int64_value rhs
      )
      PARAMS (v_i64 lhs; v_i64 rhs)
      SEP ()
    POST [ tint ]
      PROP ()
      RETURN (v_bool (lhs <? rhs))
      SEP ().

  Definition i64_nonnegative_spec : ident * funspec :=
    DECLARE C._i64_nonnegative
    WITH value : Z
    PRE [ tlong ]
      PROP (c_int64_value value)
      PARAMS (v_i64 value)
      SEP ()
    POST [ tint ]
      PROP ()
      RETURN (v_bool (0 <=? value))
      SEP ().

  Definition i64_equal_spec : ident * funspec :=
    DECLARE C._i64_equal
    WITH lhs : Z, rhs : Z
    PRE [ tlong, tlong ]
      PROP (
        c_int64_value lhs;
        c_int64_value rhs
      )
      PARAMS (v_i64 lhs; v_i64 rhs)
      SEP ()
    POST [ tint ]
      PROP ()
      RETURN (v_bool (lhs =? rhs))
      SEP ().

  Definition z_max_spec : ident * funspec :=
    DECLARE C._z_max
    WITH lhs : Z, rhs : Z
    PRE [ tlong, tlong ]
      PROP (
        c_int64_value lhs;
        c_int64_value rhs
      )
      PARAMS (v_i64 lhs; v_i64 rhs)
      SEP ()
    POST [ tlong ]
      PROP ()
      RETURN (v_i64 (z_max lhs rhs))
      SEP ().

  Definition is_runnable_spec : ident * funspec :=
    DECLARE C._is_runnable
    WITH entity_ptr : val, entity : eevdf_entity
    PRE [ tptr t_eevdf_entity ]
      PROP (eevdf_entity_c_shape entity)
      PARAMS (entity_ptr)
      SEP (eevdf_entity_data_rep Tsh entity entity_ptr)
    POST [ tint ]
      PROP ()
      RETURN (v_bool (is_runnable entity))
      SEP (eevdf_entity_data_rep Tsh entity entity_ptr).

  Definition is_active_state_spec : ident * funspec :=
    DECLARE C._is_active_state
    WITH state : eevdf_state
    PRE [ tint ]
      PROP ()
      PARAMS (eevdf_state_c_val state)
      SEP ()
    POST [ tint ]
      PROP ()
      RETURN (v_bool (is_active_state state))
      SEP ().

  Definition entity_better_values
      (candidate_deadline current_deadline candidate_thread_id
       current_thread_id : Z)
    : bool :=
    orb
      (candidate_deadline <? current_deadline)
      (andb
        (candidate_deadline =? current_deadline)
        (candidate_thread_id <? current_thread_id)).

  Definition i64_in_range
      (value : Z)
    : bool :=
    andb (c_int64_min <=? value) (value <=? c_int64_max).

  Definition checked_add_i64_ok
      (lhs rhs : Z)
    : bool :=
    i64_in_range (lhs + rhs).

  Definition checked_add_i64_overflows_spec : ident * funspec :=
    DECLARE C._checked_add_i64_overflows
    WITH lhs : Z, rhs : Z
    PRE [ tlong, tlong ]
      PROP (
        c_int64_value lhs;
        c_int64_value rhs
      )
      PARAMS (v_i64 lhs; v_i64 rhs)
      SEP ()
    POST [ tint ]
      PROP ()
      RETURN (v_bool (negb (checked_add_i64_ok lhs rhs)))
      SEP ().

  Definition checked_add_i64_spec : ident * funspec :=
    DECLARE C._checked_add_i64
    WITH lhs : Z, rhs : Z, out : val, old_out : Z
    PRE [ tlong, tlong, tptr tlong ]
      PROP (
        c_int64_value lhs;
        c_int64_value rhs;
        c_int64_value old_out
      )
      PARAMS (v_i64 lhs; v_i64 rhs; out)
      SEP (data_at Tsh tlong (v_i64 old_out) out)
    POST [ tint ]
      PROP ()
      RETURN (v_bool (checked_add_i64_ok lhs rhs))
      SEP (
        data_at Tsh tlong
          (v_i64
            (if checked_add_i64_ok lhs rhs
             then lhs + rhs
             else old_out))
          out
      ).

  Definition checked_mul_i64_nonnegative_ok
      (lhs rhs : Z)
    : bool :=
    andb (0 <=? lhs)
      (andb (0 <=? rhs)
        (negb
          (if rhs =? 0 then false else c_int64_max / rhs <? lhs))).

  Definition checked_mul_i64_nonnegative_overflows_spec
      : ident * funspec :=
    DECLARE C._checked_mul_i64_nonnegative_overflows
    WITH lhs : Z, rhs : Z
    PRE [ tlong, tlong ]
      PROP (
        c_int64_value lhs;
        c_int64_value rhs;
        0 <= lhs;
        0 <= rhs
      )
      PARAMS (v_i64 lhs; v_i64 rhs)
      SEP ()
    POST [ tint ]
      PROP ()
      RETURN (v_bool
        (if rhs =? 0 then false else c_int64_max / rhs <? lhs))
      SEP ().

  Definition checked_mul_i64_nonnegative_spec : ident * funspec :=
    DECLARE C._checked_mul_i64_nonnegative
    WITH lhs : Z, rhs : Z, out : val, old_out : Z
    PRE [ tlong, tlong, tptr tlong ]
      PROP (
        c_int64_value lhs;
        c_int64_value rhs;
        c_int64_value old_out
      )
      PARAMS (v_i64 lhs; v_i64 rhs; out)
      SEP (data_at Tsh tlong (v_i64 old_out) out)
    POST [ tint ]
      PROP ()
      RETURN (v_bool (checked_mul_i64_nonnegative_ok lhs rhs))
      SEP (
        data_at Tsh tlong
          (v_i64
            (if checked_mul_i64_nonnegative_ok lhs rhs
             then lhs * rhs
             else old_out))
          out
      ).

  Fixpoint weighted_fractional_steps
      (fuel : nat)
      (remainder weight fractional : Z)
    : Z :=
    match fuel with
    | O => fractional
    | S fuel' =>
        let fractional' := (fractional * 2)%Z in
        if Z.leb (weight - remainder) remainder then
          weighted_fractional_steps fuel'
            (remainder - (weight - remainder))%Z weight
            (fractional' + 1)%Z
        else
          weighted_fractional_steps fuel'
            (remainder + remainder)%Z weight fractional'
    end.

  Definition weighted_fractional_step_state
      (remainder weight fractional : Z)
    : Z * Z :=
    let fractional' := (fractional * 2)%Z in
    if Z.leb (weight - remainder) remainder then
      ((remainder - (weight - remainder))%Z, (fractional' + 1)%Z)
    else
      ((remainder + remainder)%Z, fractional').

  Fixpoint weighted_fractional_state_steps
      (fuel : nat)
      (remainder weight fractional : Z)
    : Z * Z :=
    match fuel with
    | O => (remainder, fractional)
    | S fuel' =>
        let state :=
          weighted_fractional_step_state remainder weight fractional in
        weighted_fractional_state_steps fuel'
          (fst state) weight (snd state)
    end.

  Definition weighted_delta_quotient
      (runtime_ns weight : Z)
    : Z :=
    (runtime_ns / weight)%Z.

  Definition weighted_delta_remainder
      (runtime_ns weight : Z)
    : Z :=
    (runtime_ns mod weight)%Z.

  Definition weighted_delta_whole
      (runtime_ns weight : Z)
    : Z :=
    (weighted_delta_quotient runtime_ns weight * 1024)%Z.

  Definition weighted_delta_fractional
      (runtime_ns weight : Z)
    : Z :=
    snd (weighted_fractional_state_steps 10
      (weighted_delta_remainder runtime_ns weight) weight 0).

  Definition weighted_delta_value
      (runtime_ns weight : Z)
    : Z :=
    (weighted_delta_whole runtime_ns weight +
      weighted_delta_fractional runtime_ns weight)%Z.

  Definition weighted_delta_ok
      (runtime_ns weight : Z)
    : bool :=
    andb (Z.leb 0 runtime_ns)
      (andb (Z.ltb 0 weight)
        (andb
          (checked_mul_i64_nonnegative_ok
            (weighted_delta_quotient runtime_ns weight) 1024)
          (checked_add_i64_ok
            (weighted_delta_whole runtime_ns weight)
            (weighted_delta_fractional runtime_ns weight)))).

  Definition weighted_fractional_10_spec : ident * funspec :=
    DECLARE C._weighted_fractional_10
    WITH remainder : Z, weight : Z, out : val
    PRE [ tlong, tlong, tptr tlong ]
      PROP (
        c_int64_value remainder;
        c_int64_value weight;
        0 <= remainder < weight;
        0 < weight
      )
      PARAMS (v_i64 remainder; v_i64 weight; out)
      SEP (data_at_ Tsh tlong out)
    POST [ tint ]
      PROP (
        c_int64_value
          (snd (weighted_fractional_state_steps 10 remainder weight 0))
      )
      RETURN (v_bool true)
      SEP (
        data_at Tsh tlong
          (v_i64
            (snd (weighted_fractional_state_steps 10 remainder weight 0)))
          out
      ).

  Definition weighted_delta_spec : ident * funspec :=
    DECLARE C._weighted_delta
    WITH runtime_ns : Z, weight : Z, out : val
    PRE [ tlong, tlong, tptr tlong ]
      PROP (
        c_int64_value runtime_ns;
        c_int64_value weight
      )
      PARAMS (v_i64 runtime_ns; v_i64 weight; out)
      SEP (data_at_ Tsh tlong out)
    POST [ tint ]
      PROP (
        weighted_delta_ok runtime_ns weight = true ->
          c_int64_value (weighted_delta_value runtime_ns weight)
      )
      RETURN (v_bool (weighted_delta_ok runtime_ns weight))
      SEP (
        if weighted_delta_ok runtime_ns weight
        then data_at Tsh tlong
          (v_i64 (weighted_delta_value runtime_ns weight)) out
        else data_at_ Tsh tlong out
      ).

  Definition weighted_slice_value
      (slice_ns weight : Z)
    : Z :=
    z_max 1 (weighted_delta_value slice_ns weight).

  Definition weighted_slice_ok
      (slice_ns weight : Z)
    : bool :=
    weighted_delta_ok slice_ns weight.

  Definition weighted_slice_spec : ident * funspec :=
    DECLARE C._weighted_slice
    WITH slice_ns : Z, weight : Z, out : val, old_out : Z
    PRE [ tlong, tlong, tptr tlong ]
      PROP (
        c_int64_value slice_ns;
        c_int64_value weight;
        c_int64_value old_out
      )
      PARAMS (v_i64 slice_ns; v_i64 weight; out)
      SEP (data_at Tsh tlong (v_i64 old_out) out)
    POST [ tint ]
      PROP (
        weighted_slice_ok slice_ns weight = true ->
          c_int64_value (weighted_slice_value slice_ns weight)
      )
      RETURN (v_bool (weighted_slice_ok slice_ns weight))
      SEP (
        data_at Tsh tlong
          (v_i64
            (if weighted_slice_ok slice_ns weight
             then weighted_slice_value slice_ns weight
             else old_out))
          out
      ).

  Definition entity_better_values_spec : ident * funspec :=
    DECLARE C._entity_better_values
    WITH candidate_deadline : Z,
      current_deadline : Z,
      candidate_thread_id : Z,
      current_thread_id : Z
    PRE [ tlong, tlong, tlong, tlong ]
      PROP (
        c_int64_value candidate_deadline;
        c_int64_value current_deadline;
        c_int64_value candidate_thread_id;
        c_int64_value current_thread_id
      )
      PARAMS (
        v_i64 candidate_deadline;
        v_i64 current_deadline;
        v_i64 candidate_thread_id;
        v_i64 current_thread_id
      )
      SEP ()
    POST [ tint ]
      PROP ()
      RETURN (v_bool
        (entity_better_values candidate_deadline current_deadline
          candidate_thread_id current_thread_id))
      SEP ().

  Definition entity_better_spec : ident * funspec :=
    DECLARE C._entity_better
    WITH candidate_ptr : val, current_ptr : val,
      candidate : eevdf_entity, current : eevdf_entity
    PRE [ tptr t_eevdf_entity, tptr t_eevdf_entity ]
      PROP (
        eevdf_entity_c_shape candidate;
        eevdf_entity_c_shape current
      )
      PARAMS (candidate_ptr; current_ptr)
      SEP (
        eevdf_entity_data_rep Tsh candidate candidate_ptr;
        eevdf_entity_data_rep Tsh current current_ptr
      )
    POST [ tint ]
      PROP ()
      RETURN (v_bool (entity_better candidate current))
      SEP (
        eevdf_entity_data_rep Tsh candidate candidate_ptr;
        eevdf_entity_data_rep Tsh current current_ptr
      ).

  Definition pacha_eevdf_empty_entity_spec : ident * funspec :=
    DECLARE C._pacha_eevdf_empty_entity
    WITH out : val
    PRE [ tptr t_eevdf_entity ]
      PROP ()
      PARAMS (out)
      SEP (data_at_ Tsh t_eevdf_entity out)
    POST [ tvoid ]
      PROP ()
      RETURN ()
      SEP (eevdf_entity_data_rep Tsh eevdf_empty_entity out).

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

  Definition pacha_eevdf_reset_spec : ident * funspec :=
    DECLARE C._pacha_eevdf_reset
    WITH rq : eevdf_runqueue, rq_ptr : val, out : val
    PRE [ tptr t_eevdf_runqueue, tptr t_eevdf_runqueue ]
      PROP (eevdf_runqueue_c_shape rq)
      PARAMS (rq_ptr; out)
      SEP (
        eevdf_runqueue_data_rep Tsh rq rq_ptr;
        data_at_ Tsh t_eevdf_runqueue out
      )
    POST [ tint ]
      PROP ()
      RETURN (v_int (eevdf_rc_c_value EevdfOk))
      SEP (
        eevdf_runqueue_data_rep Tsh rq rq_ptr;
        eevdf_runqueue_data_rep Tsh eevdf_empty_runqueue out
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

  Definition Gprog : funspecs :=
    ltac:(with_library C.prog [
      valid_positive_spec;
      i64_nonnegative_spec;
      i64_less_spec;
      i64_equal_spec;
      checked_add_i64_overflows_spec;
      checked_add_i64_spec;
      checked_mul_i64_nonnegative_overflows_spec;
      checked_mul_i64_nonnegative_spec;
      weighted_fractional_10_spec;
      weighted_delta_spec;
      weighted_slice_spec;
      z_max_spec;
      is_runnable_spec;
      is_active_state_spec;
      entity_better_values_spec;
      entity_better_spec;
      pacha_eevdf_empty_entity_spec;
      pacha_eevdf_empty_runqueue_spec;
      pacha_eevdf_reset_spec;
      pacha_eevdf_add_spec;
      pacha_eevdf_wake_spec;
      pacha_eevdf_block_spec;
      pacha_eevdf_exit_spec;
      pacha_eevdf_charge_spec;
      pacha_eevdf_mark_running_spec;
      pacha_eevdf_requeue_running_spec
    ]).
End EevdfClight.

Existing Instance EevdfClight.CompSpecs.

#[export] Instance eevdf_entity_inhabitant : Inhabitant eevdf_entity :=
  eevdf_empty_entity.

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

Lemma map_replace_nth_eevdf_entity_data :
  forall entities index entity,
    map EevdfClight.eevdf_entity_data
      (ProtocolModel.replace_nth entities index entity) =
      ProtocolModel.replace_nth
        (map EevdfClight.eevdf_entity_data entities)
        index (EevdfClight.eevdf_entity_data entity).
Proof.
  induction entities as [| head rest IH]; intros index entity.
  - destruct index; reflexivity.
  - destruct index as [| index']; simpl.
    + reflexivity.
    + rewrite IH.
      reflexivity.
Qed.

Lemma eevdf_entity_data_repeat_empty :
  map EevdfClight.eevdf_entity_data
    (repeat eevdf_empty_entity eevdf_max_entities) =
  repeat (EevdfClight.eevdf_entity_data eevdf_empty_entity)
    eevdf_max_entities.
Proof.
  apply map_repeat.
Qed.

Definition eevdf_empty_runqueue_prefix_entities
    (count : Z)
  : list (reptype EevdfClight.t_eevdf_entity) :=
  repeat (EevdfClight.eevdf_entity_data eevdf_empty_entity)
    (Z.to_nat count) ++
  repeat (default_val EevdfClight.t_eevdf_entity)
    (Z.to_nat (256 - count)).

Definition eevdf_empty_runqueue_prefix_data
    (count : Z)
  : reptype EevdfClight.t_eevdf_runqueue :=
  (eevdf_empty_runqueue_prefix_entities count,
   (default_val tulong,
    (default_val tulong,
     (default_val tlong, default_val tlong)))).

Definition eevdf_empty_runqueue_prefix_rep
    (sh : share)
    (count : Z)
    (ptr : val)
  : mpred :=
  data_at sh EevdfClight.t_eevdf_runqueue
    (eevdf_empty_runqueue_prefix_data count) ptr.

Lemma eevdf_empty_runqueue_prefix_entities_0 :
  eevdf_empty_runqueue_prefix_entities 0 =
  default_val
    (nested_field_type EevdfClight.t_eevdf_runqueue
      [StructField C._entities]).
Proof.
  unfold eevdf_empty_runqueue_prefix_entities.
  simpl.
  reflexivity.
Qed.

Lemma eevdf_empty_runqueue_prefix_data_0 :
  eevdf_empty_runqueue_prefix_data 0 =
  default_val EevdfClight.t_eevdf_runqueue.
Proof.
  unfold eevdf_empty_runqueue_prefix_data.
  rewrite eevdf_empty_runqueue_prefix_entities_0.
  simpl.
  reflexivity.
Qed.

Lemma eevdf_empty_runqueue_prefix_entities_256 :
  eevdf_empty_runqueue_prefix_entities 256 =
  map EevdfClight.eevdf_entity_data
    (er_entities eevdf_empty_runqueue).
Proof.
  unfold eevdf_empty_runqueue_prefix_entities.
  change (Z.to_nat 256) with eevdf_max_entities.
  change (Z.to_nat (256 - 256)) with 0%nat.
  rewrite app_nil_r.
  change (er_entities eevdf_empty_runqueue)
    with (repeat eevdf_empty_entity eevdf_max_entities).
  rewrite eevdf_entity_data_repeat_empty.
  reflexivity.
Qed.

Lemma eevdf_empty_runqueue_prefix_entities_length :
  forall count,
    0 <= count <= 256 ->
    length (eevdf_empty_runqueue_prefix_entities count) =
    eevdf_max_entities.
Proof.
  intros count Hrange.
  unfold eevdf_empty_runqueue_prefix_entities.
  rewrite length_app, !repeat_length.
  apply Nat2Z.inj.
  rewrite Nat2Z.inj_add, !Z2Nat.id by lia.
  change (Z.of_nat eevdf_max_entities) with 256.
  lia.
Qed.

Lemma eevdf_empty_runqueue_prefix_entities_default_lookup :
  forall count,
    0 <= count < 256 ->
    nth_error (eevdf_empty_runqueue_prefix_entities count)
      (Z.to_nat count) =
    Some (default_val EevdfClight.t_eevdf_entity).
Proof.
  intros count Hrange.
  unfold eevdf_empty_runqueue_prefix_entities.
  rewrite nth_error_app2.
  - rewrite repeat_length.
    replace (Z.to_nat count - Z.to_nat count)%nat with 0%nat
      by lia.
    destruct (Z.to_nat (256 - count)) eqn:Hsuffix.
    + pose proof (f_equal Z.of_nat Hsuffix) as HsuffixZ.
      rewrite Nat2Z.inj_0 in HsuffixZ.
      rewrite Z2Nat.id in HsuffixZ by lia.
      lia.
    + reflexivity.
  - rewrite repeat_length.
    lia.
Qed.

Lemma replace_nth_app_right :
  forall {A : Type} (left right : list A) index item,
    (length left <= index)%nat ->
    ProtocolModel.replace_nth (left ++ right) index item =
    left ++ ProtocolModel.replace_nth right (index - length left) item.
Proof.
  induction left as [| head tail IH]; intros right index item Hle.
  - simpl.
    replace (index - 0)%nat with index by lia.
    reflexivity.
  - destruct index as [| index']; simpl in *.
    + lia.
    + f_equal.
      rewrite IH by lia.
      replace (index' - length tail)%nat
        with (S index' - S (length tail))%nat by lia.
      reflexivity.
Qed.

Lemma eevdf_empty_runqueue_prefix_entities_step :
  forall count,
    0 <= count < 256 ->
    ProtocolModel.replace_nth
      (eevdf_empty_runqueue_prefix_entities count)
      (Z.to_nat count)
      (EevdfClight.eevdf_entity_data eevdf_empty_entity) =
    eevdf_empty_runqueue_prefix_entities (count + 1).
Proof.
  intros count Hrange.
  unfold eevdf_empty_runqueue_prefix_entities.
  remember (Z.to_nat count) as n.
  assert (Hcount : count = Z.of_nat n).
  {
    subst n.
    rewrite Z2Nat.id by lia.
    reflexivity.
  }
  assert (Hsuffix :
    Z.to_nat (256 - count) = S (Z.to_nat (256 - (count + 1)))).
  {
    apply Nat2Z.inj.
    rewrite Nat2Z.inj_succ, !Z2Nat.id by lia.
    lia.
  }
  assert (Hnext : Z.to_nat (count + 1) = S n).
  {
    subst n.
    apply Nat2Z.inj.
    rewrite Z2Nat.id by lia.
    rewrite Nat2Z.inj_succ, Z2Nat.id by lia.
    lia.
  }
  rewrite Hnext.
  rewrite Hsuffix.
  rewrite replace_nth_app_right.
  - rewrite repeat_length.
    replace (n - n)%nat with 0%nat by lia.
    replace (S n) with (n + 1)%nat by lia.
    rewrite repeat_app.
    simpl.
    rewrite <- app_assoc.
    reflexivity.
  - rewrite repeat_length.
    lia.
Qed.

Lemma eevdf_entity_array_update_data :
  forall entities index entity,
    (index < length entities)%nat ->
    map EevdfClight.eevdf_entity_data
      (ProtocolModel.replace_nth entities index entity) =
      upd_Znth (Z.of_nat index)
        (map EevdfClight.eevdf_entity_data entities)
        (EevdfClight.eevdf_entity_data entity).
Proof.
  intros entities index entity Hlt.
  rewrite map_replace_nth_eevdf_entity_data.
  rewrite replace_nth_as_upd_Znth.
  - reflexivity.
  - rewrite length_map.
    exact Hlt.
Qed.

Definition inactive_entity_slots_empty
    (rq : eevdf_runqueue)
  : Prop :=
  forall index entity,
    (er_entity_count rq <= index < length (er_entities rq))%nat ->
    nth_error (er_entities rq) index = Some entity ->
    entity = eevdf_empty_entity.

Lemma nth_error_replace_nth_neq :
  forall {A : Type} (items : list A) replace_index lookup_index item,
    replace_index <> lookup_index ->
    nth_error (ProtocolModel.replace_nth items replace_index item)
      lookup_index =
    nth_error items lookup_index.
Proof.
  intros A items.
  induction items as [| head rest IH];
    intros replace_index lookup_index item Hneq.
  - destruct lookup_index; reflexivity.
  - destruct replace_index as [| replace_index'];
      destruct lookup_index as [| lookup_index']; simpl.
    + contradiction.
    + reflexivity.
    + reflexivity.
    + apply IH.
      lia.
Qed.

Lemma replace_nth_length :
  forall {A : Type} (items : list A) index item,
    length (ProtocolModel.replace_nth items index item) = length items.
Proof.
  intros A items.
  induction items as [| head rest IH]; intros index item.
  - destruct index; reflexivity.
  - destruct index as [| index']; simpl.
    + reflexivity.
    + rewrite IH.
      reflexivity.
Qed.

Lemma inactive_entity_slots_empty_state_empty :
  forall rq index entity,
    inactive_entity_slots_empty rq ->
    (er_entity_count rq <= index < length (er_entities rq))%nat ->
    nth_error (er_entities rq) index = Some entity ->
    ee_state entity = EEmpty.
Proof.
  intros rq index entity Hinactive Hrange Hlookup.
  rewrite (Hinactive index entity Hrange Hlookup).
  reflexivity.
Qed.

Lemma inactive_entity_slots_empty_replace_active :
  forall rq replace_index entity,
    inactive_entity_slots_empty rq ->
    (replace_index < er_entity_count rq)%nat ->
    inactive_entity_slots_empty
      {|
        er_entities :=
          ProtocolModel.replace_nth
            (er_entities rq) replace_index entity;
        er_entity_count := er_entity_count rq;
        er_runnable_count := er_runnable_count rq;
        er_virtual_time := er_virtual_time rq;
        er_min_vruntime := er_min_vruntime rq;
      |}.
Proof.
  unfold inactive_entity_slots_empty.
  intros rq replace_index entity Hinactive Hactive index lookup_entity
    Hrange Hlookup.
  simpl in *.
  rewrite replace_nth_length in Hrange.
  rewrite nth_error_replace_nth_neq in Hlookup by lia.
  apply (Hinactive index lookup_entity Hrange Hlookup).
Qed.

Lemma empty_runqueue_inactive_entity_slots_empty :
  inactive_entity_slots_empty eevdf_empty_runqueue.
Proof.
  unfold inactive_entity_slots_empty.
  intros index entity [_ Hlt] Hlookup.
  change (er_entities eevdf_empty_runqueue)
    with (repeat eevdf_empty_entity eevdf_max_entities) in Hlookup.
  apply nth_error_In in Hlookup.
  apply repeat_spec in Hlookup.
  exact Hlookup.
Qed.

Lemma eevdf_entity_array_element_rep_unfold :
  forall sh entity rq_ptr index,
    EevdfClight.eevdf_entity_array_element_rep sh entity rq_ptr index =
    field_at sh EevdfClight.t_eevdf_runqueue
      [ArraySubsc (Z.of_nat index); StructField C._entities]
      (EevdfClight.eevdf_entity_data entity) rq_ptr.
Proof.
  reflexivity.
Qed.

Lemma eevdf_entity_array_element_field_rep_unfold :
  forall sh entity rq_ptr index,
    EevdfClight.eevdf_entity_array_element_field_rep sh entity rq_ptr index =
    field_at sh EevdfClight.t_eevdf_runqueue
      [StructField C._thread_id;
       ArraySubsc (Z.of_nat index);
       StructField C._entities]
      (EevdfClight.v_i64 (ee_thread_id entity)) rq_ptr *
    field_at sh EevdfClight.t_eevdf_runqueue
      [StructField C._generation;
       ArraySubsc (Z.of_nat index);
       StructField C._entities]
      (EevdfClight.v_i64 (ee_generation entity)) rq_ptr *
    field_at sh EevdfClight.t_eevdf_runqueue
      [StructField C._weight;
       ArraySubsc (Z.of_nat index);
       StructField C._entities]
      (EevdfClight.v_i64 (ee_weight entity)) rq_ptr *
    field_at sh EevdfClight.t_eevdf_runqueue
      [StructField C._slice_ns;
       ArraySubsc (Z.of_nat index);
       StructField C._entities]
      (EevdfClight.v_i64 (ee_slice_ns entity)) rq_ptr *
    field_at sh EevdfClight.t_eevdf_runqueue
      [StructField C._service_ns;
       ArraySubsc (Z.of_nat index);
       StructField C._entities]
      (EevdfClight.v_i64 (ee_service_ns entity)) rq_ptr *
    field_at sh EevdfClight.t_eevdf_runqueue
      [StructField C._vruntime;
       ArraySubsc (Z.of_nat index);
       StructField C._entities]
      (EevdfClight.v_i64 (ee_vruntime entity)) rq_ptr *
    field_at sh EevdfClight.t_eevdf_runqueue
      [StructField C._eligible_time;
       ArraySubsc (Z.of_nat index);
       StructField C._entities]
      (EevdfClight.v_i64 (ee_eligible_time entity)) rq_ptr *
    field_at sh EevdfClight.t_eevdf_runqueue
      [StructField C._deadline;
       ArraySubsc (Z.of_nat index);
       StructField C._entities]
      (EevdfClight.v_i64 (ee_deadline entity)) rq_ptr *
    field_at sh EevdfClight.t_eevdf_runqueue
      [StructField C._state;
       ArraySubsc (Z.of_nat index);
       StructField C._entities]
      (EevdfClight.eevdf_state_c_val (ee_state entity)) rq_ptr.
Proof.
  reflexivity.
Qed.

Lemma eevdf_entity_array_field_at_data_at :
  forall sh entities rq_ptr,
    field_at sh EevdfClight.t_eevdf_runqueue
      [StructField C._entities]
      (map EevdfClight.eevdf_entity_data entities) rq_ptr =
    data_at sh (Tarray EevdfClight.t_eevdf_entity 256 noattr)
      (map EevdfClight.eevdf_entity_data entities)
      (field_address EevdfClight.t_eevdf_runqueue
        [StructField C._entities] rq_ptr).
Proof.
  intros sh entities rq_ptr.
  rewrite field_at_data_at.
  change (nested_field_type EevdfClight.t_eevdf_runqueue
    [StructField C._entities])
    with (Tarray EevdfClight.t_eevdf_entity 256 noattr).
  reflexivity.
Qed.

Lemma eevdf_entity_value_array_field_at_data_at :
  forall sh values rq_ptr,
    field_at sh EevdfClight.t_eevdf_runqueue
      [StructField C._entities] values rq_ptr =
    data_at sh (Tarray EevdfClight.t_eevdf_entity 256 noattr)
      values
      (field_address EevdfClight.t_eevdf_runqueue
        [StructField C._entities] rq_ptr).
Proof.
  intros sh values rq_ptr.
  rewrite field_at_data_at.
  change (nested_field_type EevdfClight.t_eevdf_runqueue
    [StructField C._entities])
    with (Tarray EevdfClight.t_eevdf_entity 256 noattr).
  reflexivity.
Qed.

Definition eevdf_entity_array_data_element_ptr
    (rq_ptr : val)
    (index : nat)
  : val :=
  field_address (Tarray EevdfClight.t_eevdf_entity 256 noattr)
    [ArraySubsc (Z.of_nat index)]
    (field_address EevdfClight.t_eevdf_runqueue
      [StructField C._entities] rq_ptr).

Lemma eevdf_entity_array_subscript_offset :
  forall index,
    nested_field_offset
      (nested_field_type EevdfClight.t_eevdf_runqueue
        [StructField C._entities])
      [ArraySubsc (Z.of_nat index)] =
    (sizeof EevdfClight.t_eevdf_entity * Z.of_nat index)%Z.
Proof.
  intros index.
  simpl.
  rewrite Z.add_0_l.
  reflexivity.
Qed.

Lemma eevdf_entity_array_element_address :
  forall rq_ptr index,
    field_compatible EevdfClight.t_eevdf_runqueue
      [StructField C._entities] rq_ptr ->
    field_compatible
      (nested_field_type EevdfClight.t_eevdf_runqueue
        [StructField C._entities])
      [ArraySubsc (Z.of_nat index)]
      (field_address EevdfClight.t_eevdf_runqueue
        [StructField C._entities] rq_ptr) ->
    field_address EevdfClight.t_eevdf_runqueue
      [ArraySubsc (Z.of_nat index); StructField C._entities] rq_ptr =
    offset_val (sizeof EevdfClight.t_eevdf_entity * Z.of_nat index)%Z
      (field_address EevdfClight.t_eevdf_runqueue
        [StructField C._entities] rq_ptr).
Proof.
  intros rq_ptr index Hentities_compat Harray_compat.
  rewrite field_address_app
    with (gfsA := [StructField C._entities])
         (gfsB := [ArraySubsc (Z.of_nat index)]).
  rewrite field_address_offset by exact Harray_compat.
  rewrite eevdf_entity_array_subscript_offset.
  reflexivity.
Qed.

Lemma eevdf_entity_array_element_sem_add :
  forall rq_ptr index,
    field_compatible EevdfClight.t_eevdf_runqueue
      [StructField C._entities] rq_ptr ->
    field_compatible
      (nested_field_type EevdfClight.t_eevdf_runqueue
        [StructField C._entities])
      [ArraySubsc (Z.of_nat index)]
      (field_address EevdfClight.t_eevdf_runqueue
        [StructField C._entities] rq_ptr) ->
    force_val
      (sem_add_ptr_long EevdfClight.t_eevdf_entity
        (field_address EevdfClight.t_eevdf_runqueue
          [StructField C._entities] rq_ptr)
        (EevdfClight.v_size index)) =
    field_address EevdfClight.t_eevdf_runqueue
      [ArraySubsc (Z.of_nat index); StructField C._entities] rq_ptr.
Proof.
  intros rq_ptr index Hentities_compat Harray_compat.
  unfold EevdfClight.v_size.
  rewrite sem_add_pl_ptr_special.
  - rewrite eevdf_entity_array_element_address by eassumption.
    reflexivity.
  - cbv.
    reflexivity.
  - apply field_address_isptr.
    exact Hentities_compat.
Qed.

Lemma eevdf_entity_array_element_ramif :
  forall sh entities entity rq_ptr index,
    length entities = eevdf_max_entities ->
    nth_error entities index = Some entity ->
    (index < eevdf_max_entities)%nat ->
    field_at sh EevdfClight.t_eevdf_runqueue
      [StructField C._entities]
      (map EevdfClight.eevdf_entity_data entities) rq_ptr |--
    EevdfClight.eevdf_entity_data_rep sh entity
      (EevdfClight.eevdf_entity_array_element_ptr rq_ptr index) *
    (EevdfClight.eevdf_entity_data_rep sh entity
      (EevdfClight.eevdf_entity_array_element_ptr rq_ptr index) -*
     field_at sh EevdfClight.t_eevdf_runqueue
      [StructField C._entities]
      (map EevdfClight.eevdf_entity_data entities) rq_ptr).
Proof.
  intros sh entities entity rq_ptr index Hlen Hlookup Hlt.
  assert (Hfield_array :
    field_at sh EevdfClight.t_eevdf_runqueue
      [StructField C._entities]
      (map EevdfClight.eevdf_entity_data entities) rq_ptr =
    array_at sh EevdfClight.t_eevdf_runqueue
      [StructField C._entities] 0 256
      (map EevdfClight.eevdf_entity_data entities) rq_ptr).
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
      with (t0 := EevdfClight.t_eevdf_entity)
           (n := 256)
           (a := noattr)
           (v0 := EevdfClight.eevdf_entity_data entity).
    - reflexivity.
    - pose proof (proj1 (Nat2Z.inj_lt index eevdf_max_entities) Hlt)
        as HltZ.
      change (Z.of_nat eevdf_max_entities) with 256 in HltZ.
      split.
      + apply Nat2Z.is_nonneg.
      + exact HltZ.
    - rewrite Z.sub_0_r.
      rewrite Znth_map.
      rewrite (Znth_of_nat_nth_error entities index entity Hlookup).
      + apply JMeq_refl.
      + pose proof (proj1 (Nat2Z.inj_lt index eevdf_max_entities) Hlt)
          as HltZ.
        change (Z.of_nat eevdf_max_entities) with 256 in HltZ.
        rewrite Zlength_correct, Hlen.
        lia.
  }
  apply sepcon_derives.
  - unfold EevdfClight.eevdf_entity_data_rep,
      EevdfClight.eevdf_entity_array_element_ptr.
    rewrite field_at_data_at.
    apply derives_refl.
  - apply allp_left with (EevdfClight.eevdf_entity_data entity).
    apply allp_left with (EevdfClight.eevdf_entity_data entity).
    rewrite prop_imp by apply JMeq_refl.
    apply wand_derives.
    + unfold EevdfClight.eevdf_entity_data_rep,
        EevdfClight.eevdf_entity_array_element_ptr.
      rewrite field_at_data_at.
      apply derives_refl.
    + rewrite Z.sub_0_r.
      erewrite upd_Znth_triv.
      * rewrite <- Hfield_array.
        apply derives_refl.
      * rewrite Zlength_map, Zlength_correct, Hlen.
        pose proof (proj1 (Nat2Z.inj_lt index eevdf_max_entities) Hlt)
          as HltZ.
        change (Z.of_nat eevdf_max_entities) with 256 in HltZ.
        split.
        -- apply Nat2Z.is_nonneg.
        -- exact HltZ.
      * rewrite Znth_map.
        -- rewrite (Znth_of_nat_nth_error entities index entity Hlookup).
           reflexivity.
        -- rewrite Zlength_correct, Hlen.
           pose proof (proj1 (Nat2Z.inj_lt index eevdf_max_entities) Hlt)
             as HltZ.
           change (Z.of_nat eevdf_max_entities) with 256 in HltZ.
           split.
           ++ apply Nat2Z.is_nonneg.
           ++ exact HltZ.
Qed.

Lemma eevdf_entity_array_value_ramif :
  forall sh values old_value new_value rq_ptr index,
    length values = eevdf_max_entities ->
    nth_error values index = Some old_value ->
    (index < eevdf_max_entities)%nat ->
    field_at sh EevdfClight.t_eevdf_runqueue
      [StructField C._entities] values rq_ptr |--
    data_at sh EevdfClight.t_eevdf_entity old_value
      (eevdf_entity_array_data_element_ptr rq_ptr index) *
    (data_at sh EevdfClight.t_eevdf_entity new_value
      (eevdf_entity_array_data_element_ptr rq_ptr index) -*
     field_at sh EevdfClight.t_eevdf_runqueue
      [StructField C._entities]
      (ProtocolModel.replace_nth values index new_value) rq_ptr).
Proof.
  intros sh values old_value new_value rq_ptr index Hlen Hlookup Hlt.
  rewrite eevdf_entity_value_array_field_at_data_at.
  eapply derives_trans.
  {
    apply SingletonHole.array_with_hole_intro.
    pose proof (proj1 (Nat2Z.inj_lt index eevdf_max_entities) Hlt)
      as HltZ.
    change (Z.of_nat eevdf_max_entities) with 256 in HltZ.
    split.
    - apply Nat2Z.is_nonneg.
    - exact HltZ.
  }
  apply sepcon_derives.
  - apply derives_refl'.
    rewrite (Znth_of_nat_nth_error values index old_value Hlookup)
      by (pose proof (proj1 (Nat2Z.inj_lt index eevdf_max_entities) Hlt)
            as HltZ;
          change (Z.of_nat eevdf_max_entities) with 256 in HltZ;
          rewrite Zlength_correct, Hlen;
          lia).
    reflexivity.
  - unfold SingletonHole.array_with_hole.
    apply allp_left with new_value.
    apply wand_derives.
    + apply derives_refl.
    + rewrite replace_nth_as_upd_Znth.
      * rewrite eevdf_entity_value_array_field_at_data_at.
        apply derives_refl.
      * rewrite Hlen.
        exact Hlt.
Qed.

Lemma body_valid_positive :
  semax_body
    EevdfClight.Vprog
    EevdfClight.Gprog
    C.f_valid_positive
    EevdfClight.valid_positive_spec.
Proof.
  start_function.
  forward.
  entailer!.
  unfold EevdfClight.v_bool, EevdfClight.v_int, EevdfClight.v_i64.
  unfold sem_cmp, sem_binarith, sem_cast, sem_cast_i2i.
  simpl.
  f_equal.
  f_equal.
  unfold Int64.lt.
  assert (Hvalue_range :
    Int64.min_signed <= value <= Int64.max_signed)
    by (change Int64.min_signed with (-9223372036854775808);
        change Int64.max_signed with 9223372036854775807;
        unfold c_int64_value, c_int64_min, c_int64_max in *; lia).
  change (Int.signed (Int.repr 0)) with 0.
  rewrite (Int64.signed_repr 0) by
    (change Int64.min_signed with (-9223372036854775808);
     change Int64.max_signed with 9223372036854775807; lia).
  rewrite (Int64.signed_repr value) by exact Hvalue_range.
  destruct (zlt 0 value) as [Hlt | Hnlt].
  - destruct (0 <? value) eqn:Hcmp.
    + reflexivity.
    + apply Z.ltb_ge in Hcmp; lia.
  - destruct (0 <? value) eqn:Hcmp.
    + apply Z.ltb_lt in Hcmp; lia.
    + reflexivity.
Qed.

Lemma body_i64_nonnegative :
  semax_body
    EevdfClight.Vprog
    EevdfClight.Gprog
    C.f_i64_nonnegative
    EevdfClight.i64_nonnegative_spec.
Proof.
  start_function.
  forward.
  entailer!.
  unfold EevdfClight.v_bool, EevdfClight.v_int, EevdfClight.v_i64.
  unfold sem_cmp, sem_binarith, sem_cast, sem_cast_i2i.
  simpl.
  f_equal.
  f_equal.
  unfold Int64.lt.
  assert (Hvalue_range :
    Int64.min_signed <= value <= Int64.max_signed)
    by (change Int64.min_signed with (-9223372036854775808);
        change Int64.max_signed with 9223372036854775807;
        unfold c_int64_value, c_int64_min, c_int64_max in *; lia).
  change (Int.signed (Int.repr 0)) with 0.
  rewrite (Int64.signed_repr 0) by
    (change Int64.min_signed with (-9223372036854775808);
     change Int64.max_signed with 9223372036854775807; lia).
  rewrite (Int64.signed_repr value) by exact Hvalue_range.
  destruct (zlt value 0) as [Hlt | Hnlt].
  - destruct (0 <=? value) eqn:Hcmp.
    + apply Z.leb_le in Hcmp; lia.
    + reflexivity.
  - destruct (0 <=? value) eqn:Hcmp.
    + reflexivity.
    + apply Z.leb_gt in Hcmp; lia.
Qed.

Lemma body_i64_less :
  semax_body
    EevdfClight.Vprog
    EevdfClight.Gprog
    C.f_i64_less
    EevdfClight.i64_less_spec.
Proof.
  start_function.
  forward.
  entailer!.
  unfold EevdfClight.v_bool, EevdfClight.v_int, EevdfClight.v_i64.
  unfold sem_cmp, sem_binarith, sem_cast, sem_cast_i2i.
  simpl.
  f_equal.
  f_equal.
  unfold Int64.lt.
  assert (Hlhs_range :
    Int64.min_signed <= lhs <= Int64.max_signed)
    by (change Int64.min_signed with (-9223372036854775808);
        change Int64.max_signed with 9223372036854775807;
        unfold c_int64_value, c_int64_min, c_int64_max in *; lia).
  assert (Hrhs_range :
    Int64.min_signed <= rhs <= Int64.max_signed)
    by (change Int64.min_signed with (-9223372036854775808);
        change Int64.max_signed with 9223372036854775807;
        unfold c_int64_value, c_int64_min, c_int64_max in *; lia).
  rewrite (Int64.signed_repr lhs) by exact Hlhs_range.
  rewrite (Int64.signed_repr rhs) by exact Hrhs_range.
  destruct (zlt lhs rhs) as [Hlt | Hnlt].
  - destruct (lhs <? rhs) eqn:Hcmp.
    + reflexivity.
    + apply Z.ltb_ge in Hcmp; lia.
  - destruct (lhs <? rhs) eqn:Hcmp.
    + apply Z.ltb_lt in Hcmp; lia.
    + reflexivity.
Qed.

Lemma body_i64_equal :
  semax_body
    EevdfClight.Vprog
    EevdfClight.Gprog
    C.f_i64_equal
    EevdfClight.i64_equal_spec.
Proof.
  start_function.
  forward.
  entailer!.
  unfold EevdfClight.v_bool, EevdfClight.v_int, EevdfClight.v_i64.
  unfold sem_cmp, sem_binarith, sem_cast, sem_cast_i2i.
  simpl.
  f_equal.
  f_equal.
  rewrite Int64.eq_signed.
  assert (Hlhs_range :
    Int64.min_signed <= lhs <= Int64.max_signed)
    by (change Int64.min_signed with (-9223372036854775808);
        change Int64.max_signed with 9223372036854775807;
        unfold c_int64_value, c_int64_min, c_int64_max in *; lia).
  assert (Hrhs_range :
    Int64.min_signed <= rhs <= Int64.max_signed)
    by (change Int64.min_signed with (-9223372036854775808);
        change Int64.max_signed with 9223372036854775807;
        unfold c_int64_value, c_int64_min, c_int64_max in *; lia).
  rewrite (Int64.signed_repr lhs) by exact Hlhs_range.
  rewrite (Int64.signed_repr rhs) by exact Hrhs_range.
  destruct (zeq lhs rhs) as [Heq | Hneq].
  - subst.
    destruct (rhs =? rhs) eqn:Hcmp.
    + reflexivity.
    + apply Z.eqb_neq in Hcmp; contradiction.
  - destruct (lhs =? rhs) eqn:Hcmp.
    + apply Z.eqb_eq in Hcmp; contradiction.
    + reflexivity.
Qed.

Lemma body_z_max :
  semax_body
    EevdfClight.Vprog
    EevdfClight.Gprog
    C.f_z_max
    EevdfClight.z_max_spec.
Proof.
  start_function.
  forward_call (lhs, rhs).
  destruct (lhs <? rhs) eqn:Hlt.
  - forward_if.
    + forward.
      unfold z_max.
      rewrite Hlt.
      entailer!.
    + exfalso.
	           match goal with
	           | Hbad : Int.zero <> Int.zero |- _ => exact (Hbad eq_refl)
	           | Hbad : Int.repr 0 <> Int.zero |- _ =>
	               try change (Int.repr 0) with Int.zero in Hbad;
	               exact (Hbad eq_refl)
	           | Hbad : Int.repr 1 = Int.zero |- _ =>
	               try change (Int.repr 1) with Int.one in Hbad;
	               exact (Int.one_not_zero Hbad)
	           end.
  - forward_if.
    + exfalso.
      match goal with
      | Hbad : Int.zero <> Int.zero |- _ => exact (Hbad eq_refl)
      | Hbad : Int.repr 0 <> Int.zero |- _ =>
          try change (Int.repr 0) with Int.zero in Hbad;
          exact (Hbad eq_refl)
      end.
    + forward.
      unfold z_max.
      rewrite Hlt.
      entailer!.
Qed.

Lemma body_is_runnable :
  semax_body
    EevdfClight.Vprog
    EevdfClight.Gprog
    C.f_is_runnable
    EevdfClight.is_runnable_spec.
Proof.
  start_function.
  destruct entity as
    [thread_id generation weight slice_ns service_ns vruntime
     eligible_time deadline state].
  forward.
  forward.
  entailer!.
  unfold EevdfClight.v_bool, EevdfClight.v_int, is_runnable.
  destruct state; reflexivity.
Qed.

Lemma body_is_active_state :
  semax_body
    EevdfClight.Vprog
    EevdfClight.Gprog
    C.f_is_active_state
    EevdfClight.is_active_state_spec.
Proof.
  start_function.
  forward.
  entailer!.
  unfold EevdfClight.v_bool, EevdfClight.v_int, is_active_state.
  destruct state; reflexivity.
Qed.

Lemma checked_add_i64_ok_true_range :
  forall lhs rhs,
    EevdfClight.checked_add_i64_ok lhs rhs = true ->
    c_int64_value (lhs + rhs).
Proof.
  intros lhs rhs Hok.
  unfold EevdfClight.checked_add_i64_ok, EevdfClight.i64_in_range in Hok.
  apply andb_prop in Hok as [Hmin Hmax].
  apply Z.leb_le in Hmin.
  apply Z.leb_le in Hmax.
  unfold c_int64_value.
  split; assumption.
Qed.

Lemma checked_add_i64_overflows_positive :
  forall lhs rhs,
    c_int64_value lhs ->
    c_int64_value rhs ->
    0 < rhs ->
    (c_int64_max - rhs <? lhs) =
      negb (EevdfClight.checked_add_i64_ok lhs rhs).
Proof.
  intros lhs rhs Hlhs Hrhs Hpos.
  unfold EevdfClight.checked_add_i64_ok, EevdfClight.i64_in_range.
  unfold c_int64_value in *.
  destruct (c_int64_max - rhs <? lhs) eqn:Hover.
  - apply Z.ltb_lt in Hover.
    symmetry.
    apply Bool.negb_true_iff.
    apply Bool.andb_false_intro2.
    apply Z.leb_gt.
    lia.
  - apply Z.ltb_ge in Hover.
    symmetry.
    apply Bool.negb_false_iff.
    apply andb_true_intro.
    split; apply Z.leb_le; lia.
Qed.

Lemma checked_add_i64_overflows_negative :
  forall lhs rhs,
    c_int64_value lhs ->
    c_int64_value rhs ->
    rhs < 0 ->
    (lhs <? c_int64_min - rhs) =
      negb (EevdfClight.checked_add_i64_ok lhs rhs).
Proof.
  intros lhs rhs Hlhs Hrhs Hneg.
  unfold EevdfClight.checked_add_i64_ok, EevdfClight.i64_in_range.
  unfold c_int64_value in *.
  destruct (lhs <? c_int64_min - rhs) eqn:Hover.
  - apply Z.ltb_lt in Hover.
    symmetry.
    apply Bool.negb_true_iff.
    apply Bool.andb_false_intro1.
    apply Z.leb_gt.
    lia.
  - apply Z.ltb_ge in Hover.
    symmetry.
    apply Bool.negb_false_iff.
    apply andb_true_intro.
    split; apply Z.leb_le; lia.
Qed.

Lemma checked_add_i64_ok_zero_rhs :
  forall lhs rhs,
    c_int64_value lhs ->
    rhs = 0 ->
    EevdfClight.checked_add_i64_ok lhs rhs = true.
Proof.
  intros lhs rhs Hlhs ->.
  unfold EevdfClight.checked_add_i64_ok, EevdfClight.i64_in_range.
  unfold c_int64_value in Hlhs.
  apply andb_true_intro.
  split; apply Z.leb_le; lia.
Qed.

Lemma body_checked_add_i64 :
  semax_body
    EevdfClight.Vprog
    EevdfClight.Gprog
    C.f_checked_add_i64
    EevdfClight.checked_add_i64_spec.
Proof.
  start_function.
  forward_call (lhs, rhs).
  destruct (EevdfClight.checked_add_i64_ok lhs rhs) eqn:Hok.
  - forward_if.
    + exfalso.
      match goal with
      | Hbad : Int.repr 0 <> Int.zero |- _ =>
          try change (Int.repr 0) with Int.zero in Hbad;
          exact (Hbad eq_refl)
      | Hbad : Int.zero <> Int.zero |- _ => exact (Hbad eq_refl)
      end.
    + forward.
      entailer!.
      apply checked_add_i64_ok_true_range in Hok.
      unfold EevdfClight.v_i64.
      unfold c_int64_value, c_int64_min, c_int64_max in Hok.
      assert (Hlhs_range :
        Int64.min_signed <= lhs <= Int64.max_signed).
      {
        match goal with
        | H : c_int64_value lhs |- _ =>
            unfold c_int64_value, c_int64_min, c_int64_max in H;
            change Int64.min_signed with (-9223372036854775808);
            change Int64.max_signed with 9223372036854775807;
            lia
        end.
      }
      assert (Hrhs_range :
        Int64.min_signed <= rhs <= Int64.max_signed).
      {
        match goal with
        | H : c_int64_value rhs |- _ =>
            unfold c_int64_value, c_int64_min, c_int64_max in H;
            change Int64.min_signed with (-9223372036854775808);
            change Int64.max_signed with 9223372036854775807;
            lia
        end.
      }
      rewrite (Int64.signed_repr lhs) by exact Hlhs_range.
	      rewrite (Int64.signed_repr rhs) by exact Hrhs_range.
	      change Int64.min_signed with (-9223372036854775808).
	      change Int64.max_signed with 9223372036854775807.
	      lia.
	      forward.
  - forward_if.
    + forward.
	    + exfalso.
	      match goal with
	      | Hbad : Int.zero <> Int.zero |- _ => exact (Hbad eq_refl)
	      | Hbad : Int.repr 0 <> Int.zero |- _ =>
	          try change (Int.repr 0) with Int.zero in Hbad;
	          exact (Hbad eq_refl)
	      | Hbad : Int.repr 1 = Int.zero |- _ =>
	          try change (Int.repr 1) with Int.one in Hbad;
	          exact (Int.one_not_zero Hbad)
	      end.
Qed.

Lemma checked_mul_i64_nonnegative_ok_true_range :
  forall lhs rhs,
    EevdfClight.checked_mul_i64_nonnegative_ok lhs rhs = true ->
    c_int64_value (lhs * rhs).
Proof.
  intros lhs rhs Hok.
  unfold EevdfClight.checked_mul_i64_nonnegative_ok in Hok.
  apply andb_prop in Hok as [Hlhs_nonneg Hrest].
  apply andb_prop in Hrest as [Hrhs_nonneg Hover].
  apply Z.leb_le in Hlhs_nonneg.
  apply Z.leb_le in Hrhs_nonneg.
  apply Bool.negb_true_iff in Hover.
  destruct (rhs =? 0) eqn:Hrhs_zero.
  - apply Z.eqb_eq in Hrhs_zero.
    subst rhs.
    unfold c_int64_value, c_int64_min, c_int64_max.
    lia.
  - apply Z.eqb_neq in Hrhs_zero.
    apply Z.ltb_ge in Hover.
    assert (Hrhs_pos : 0 < rhs) by lia.
    assert (Hupper : lhs * rhs <= c_int64_max).
    {
      replace (lhs * rhs)%Z with (rhs * lhs)%Z by lia.
      eapply Z.le_trans.
      - apply Z.mul_le_mono_nonneg_l.
        + lia.
        + exact Hover.
      - apply Z.mul_div_le.
        exact Hrhs_pos.
    }
    unfold c_int64_value, c_int64_min, c_int64_max.
    split.
    + assert (0 <= lhs * rhs)%Z by
        (apply Z.mul_nonneg_nonneg; lia).
      lia.
    + exact Hupper.
Qed.

Lemma checked_mul_i64_nonnegative_ok_false_lhs :
  forall lhs rhs,
    (0 <=? lhs) = false ->
    EevdfClight.checked_mul_i64_nonnegative_ok lhs rhs = false.
Proof.
  intros lhs rhs Hlhs.
  unfold EevdfClight.checked_mul_i64_nonnegative_ok.
  rewrite Hlhs.
  reflexivity.
Qed.

Lemma checked_mul_i64_nonnegative_ok_false_rhs :
  forall lhs rhs,
    (0 <=? lhs) = true ->
    (0 <=? rhs) = false ->
    EevdfClight.checked_mul_i64_nonnegative_ok lhs rhs = false.
Proof.
  intros lhs rhs Hlhs Hrhs.
  unfold EevdfClight.checked_mul_i64_nonnegative_ok.
  rewrite Hlhs, Hrhs.
  reflexivity.
Qed.

Lemma checked_mul_i64_nonnegative_ok_false_overflow :
  forall lhs rhs,
    (0 <=? lhs) = true ->
    (0 <=? rhs) = true ->
    (if rhs =? 0 then false else c_int64_max / rhs <? lhs) = true ->
    EevdfClight.checked_mul_i64_nonnegative_ok lhs rhs = false.
Proof.
  intros lhs rhs Hlhs Hrhs Hover.
  unfold EevdfClight.checked_mul_i64_nonnegative_ok.
  rewrite Hlhs, Hrhs, Hover.
  reflexivity.
Qed.

Lemma checked_mul_i64_nonnegative_ok_true_no_overflow :
  forall lhs rhs,
    (0 <=? lhs) = true ->
    (0 <=? rhs) = true ->
    (if rhs =? 0 then false else c_int64_max / rhs <? lhs) = false ->
    EevdfClight.checked_mul_i64_nonnegative_ok lhs rhs = true.
Proof.
  intros lhs rhs Hlhs Hrhs Hover.
  unfold EevdfClight.checked_mul_i64_nonnegative_ok.
  rewrite Hlhs, Hrhs, Hover.
  reflexivity.
Qed.

Lemma weighted_fractional_step_state_remainder_range :
  forall remainder weight fractional,
    0 < weight ->
    0 <= remainder < weight ->
    0 <= fst (EevdfClight.weighted_fractional_step_state
      remainder weight fractional) < weight.
Proof.
  intros remainder weight fractional Hweight Hremainder.
  unfold EevdfClight.weighted_fractional_step_state.
  destruct (Z.leb (weight - remainder) remainder) eqn:Htake.
  - apply Z.leb_le in Htake.
    simpl.
    lia.
  - apply Z.leb_gt in Htake.
    simpl.
    lia.
Qed.

Lemma weighted_fractional_state_steps_remainder_range :
  forall fuel remainder weight fractional,
    0 < weight ->
    0 <= remainder < weight ->
    0 <= fst (EevdfClight.weighted_fractional_state_steps
      fuel remainder weight fractional) < weight.
Proof.
  induction fuel as [| fuel IH];
    intros remainder weight fractional Hweight Hremainder.
  - simpl.
    exact Hremainder.
  - simpl.
    apply IH.
    + exact Hweight.
    + apply weighted_fractional_step_state_remainder_range;
        assumption.
Qed.

Lemma weighted_fractional_state_steps_app :
  forall fuel extra remainder weight fractional,
    EevdfClight.weighted_fractional_state_steps
      (fuel + extra) remainder weight fractional =
    let state :=
      EevdfClight.weighted_fractional_state_steps
        fuel remainder weight fractional in
    EevdfClight.weighted_fractional_state_steps
      extra (fst state) weight (snd state).
Proof.
  induction fuel as [| fuel IH];
    intros extra remainder weight fractional.
  - reflexivity.
  - simpl.
    remember (EevdfClight.weighted_fractional_step_state
      remainder weight fractional) as state.
    exact (IH extra (fst state) weight (snd state)).
Qed.

Lemma weighted_fractional_state_steps_succ :
  forall fuel remainder weight fractional,
    EevdfClight.weighted_fractional_state_steps
      (S fuel) remainder weight fractional =
    EevdfClight.weighted_fractional_step_state
      (fst (EevdfClight.weighted_fractional_state_steps
        fuel remainder weight fractional))
      weight
      (snd (EevdfClight.weighted_fractional_state_steps
        fuel remainder weight fractional)).
Proof.
  intros fuel remainder weight fractional.
  replace (S fuel) with (fuel + 1)%nat by lia.
  rewrite weighted_fractional_state_steps_app.
  simpl.
  destruct (EevdfClight.weighted_fractional_step_state
    (fst (EevdfClight.weighted_fractional_state_steps
      fuel remainder weight fractional))
    weight
    (snd (EevdfClight.weighted_fractional_state_steps
      fuel remainder weight fractional))).
  reflexivity.
Qed.

Lemma weighted_fractional_step_state_fractional_nonnegative :
  forall remainder weight fractional,
    0 <= fractional ->
    0 <= snd (EevdfClight.weighted_fractional_step_state
      remainder weight fractional).
Proof.
  intros remainder weight fractional Hfractional.
  unfold EevdfClight.weighted_fractional_step_state.
  destruct (Z.leb (weight - remainder) remainder); simpl; lia.
Qed.

Lemma weighted_fractional_state_steps_fractional_nonnegative :
  forall fuel remainder weight fractional,
    0 <= fractional ->
    0 <= snd (EevdfClight.weighted_fractional_state_steps
      fuel remainder weight fractional).
Proof.
  induction fuel as [| fuel IH];
    intros remainder weight fractional Hfractional.
  - simpl.
    exact Hfractional.
  - simpl.
    apply IH.
    apply weighted_fractional_step_state_fractional_nonnegative.
    exact Hfractional.
Qed.

Lemma weighted_fractional_step_state_fractional_upper :
  forall remainder weight fractional,
    snd (EevdfClight.weighted_fractional_step_state
      remainder weight fractional) <= (fractional * 2 + 1)%Z.
Proof.
  intros remainder weight fractional.
  unfold EevdfClight.weighted_fractional_step_state.
  destruct (Z.leb (weight - remainder) remainder); simpl; lia.
Qed.

Lemma weighted_fractional_state_steps_fractional_upper :
  forall fuel remainder weight fractional,
    0 <= fractional ->
    snd (EevdfClight.weighted_fractional_state_steps
      fuel remainder weight fractional) <=
      (fractional * 2 ^ Z.of_nat fuel +
        (2 ^ Z.of_nat fuel - 1))%Z.
Proof.
  induction fuel as [| fuel IH];
    intros remainder weight fractional Hfractional.
  - simpl.
    lia.
  - simpl.
    remember (EevdfClight.weighted_fractional_step_state
      remainder weight fractional) as state.
    pose proof (weighted_fractional_step_state_fractional_nonnegative
      remainder weight fractional Hfractional) as Hnext_nonnegative.
    rewrite <- Heqstate in Hnext_nonnegative.
    pose proof (weighted_fractional_step_state_fractional_upper
      remainder weight fractional) as Hnext_upper.
    rewrite <- Heqstate in Hnext_upper.
    pose proof (IH (fst state) weight (snd state) Hnext_nonnegative)
      as Hupper.
    assert (Hpow_succ :
      (2 ^ Z.of_nat (S fuel) = 2 * 2 ^ Z.of_nat fuel)%Z).
    {
      rewrite Nat2Z.inj_succ.
      rewrite Z.pow_succ_r by lia.
      reflexivity.
    }
    replace (Z.pow_pos 2 (Pos.of_succ_nat fuel))
      with (2 * 2 ^ Z.of_nat fuel)%Z by
      (change (Z.pow_pos 2 (Pos.of_succ_nat fuel))
        with (2 ^ Z.of_nat (S fuel))%Z;
       symmetry;
       exact Hpow_succ).
    nia.
Qed.

Lemma weighted_fractional_state_steps_fractional_range_10 :
  forall fuel remainder weight,
    (fuel <= 10)%nat ->
    0 <= snd (EevdfClight.weighted_fractional_state_steps
      fuel remainder weight 0) <= 1023.
Proof.
  intros fuel remainder weight Hfuel.
  split.
  - apply weighted_fractional_state_steps_fractional_nonnegative.
    lia.
  - pose proof (weighted_fractional_state_steps_fractional_upper
      fuel remainder weight 0) as Hupper.
    assert (Hpow : (2 ^ Z.of_nat fuel <= 2 ^ 10)%Z).
    {
      apply Z.pow_le_mono_r; lia.
    }
    change (2 ^ 10) with 1024 in Hpow.
    simpl in Hupper.
    lia.
Qed.

Lemma weighted_delta_ok_true_range :
  forall runtime_ns weight,
    EevdfClight.weighted_delta_ok runtime_ns weight = true ->
    c_int64_value (EevdfClight.weighted_delta_value runtime_ns weight).
Proof.
  intros runtime_ns weight Hok.
  unfold EevdfClight.weighted_delta_ok in Hok.
  apply andb_prop in Hok as [_ Hrest].
  apply andb_prop in Hrest as [_ Hchecks].
  apply andb_prop in Hchecks as [_ Hadd].
  unfold EevdfClight.weighted_delta_value.
  unfold EevdfClight.weighted_delta_whole in Hadd.
  unfold EevdfClight.weighted_delta_fractional in Hadd.
  unfold EevdfClight.weighted_delta_quotient in Hadd.
  unfold EevdfClight.weighted_delta_remainder in Hadd.
  apply checked_add_i64_ok_true_range.
  exact Hadd.
Qed.

Lemma weighted_delta_ok_false_runtime :
  forall runtime_ns weight,
    (0 <=? runtime_ns) = false ->
    EevdfClight.weighted_delta_ok runtime_ns weight = false.
Proof.
  intros runtime_ns weight Hruntime.
  unfold EevdfClight.weighted_delta_ok.
  rewrite Hruntime.
  reflexivity.
Qed.

Lemma body_weighted_fractional_10 :
  semax_body
    EevdfClight.Vprog
    EevdfClight.Gprog
    C.f_weighted_fractional_10
    EevdfClight.weighted_fractional_10_spec.
Proof.
  start_function.
  forward.
  forward_for_simple_bound 10
    (EX i : Z,
      PROP ()
      LOCAL (
        temp C._fractional
          (EevdfClight.v_i64
            (snd (EevdfClight.weighted_fractional_state_steps
              (Z.to_nat i) remainder weight 0)));
        temp C._remainder
          (EevdfClight.v_i64
            (fst (EevdfClight.weighted_fractional_state_steps
              (Z.to_nat i) remainder weight 0)));
        temp C._weight (EevdfClight.v_i64 weight);
        temp C._out out
      )
      SEP (data_at_ Tsh tlong out)).
  - entailer!.
  - forward.
    entailer!.
    pose proof (weighted_fractional_state_steps_fractional_range_10
      (Z.to_nat i) remainder weight) as Hfractional_range.
    assert (Hfuel : (Z.to_nat i <= 10)%nat).
    {
      apply Nat2Z.inj_le.
      rewrite Z2Nat.id by lia.
      lia.
    }
    specialize (Hfractional_range Hfuel).
    rewrite Int64.signed_repr
      by (change Int64.min_signed with (-9223372036854775808);
          change Int64.max_signed with 9223372036854775807;
          lia).
    change Int64.min_signed with (-9223372036854775808).
    change Int64.max_signed with 9223372036854775807.
    lia.
    pose proof (weighted_fractional_state_steps_fractional_range_10
      (Z.to_nat i) remainder weight) as Hfractional_range.
    assert (Hfuel : (Z.to_nat i <= 10)%nat).
    {
      apply Nat2Z.inj_le.
      rewrite Z2Nat.id by lia.
      lia.
    }
    specialize (Hfractional_range Hfuel).
    change (Int.signed (Int.repr 2)) with 2.
    rewrite Int64.mul_signed.
    rewrite (Int64.signed_repr
      (snd (EevdfClight.weighted_fractional_state_steps
        (Z.to_nat i) remainder weight 0)))
      by (change Int64.min_signed with (-9223372036854775808);
          change Int64.max_signed with 9223372036854775807;
          lia).
    rewrite (Int64.signed_repr 2)
      by (change Int64.min_signed with (-9223372036854775808);
          change Int64.max_signed with 9223372036854775807;
          lia).
    forward_if.
    + entailer!.
      pose proof (weighted_fractional_state_steps_remainder_range
        (Z.to_nat i) remainder weight 0) as Hremainder_current.
      specialize (Hremainder_current ltac:(assumption) ltac:(assumption)).
      rewrite (Int64.signed_repr weight)
        by (change Int64.min_signed with (-9223372036854775808);
            change Int64.max_signed with 9223372036854775807;
            unfold c_int64_value, c_int64_min, c_int64_max in *;
            lia).
      rewrite (Int64.signed_repr
        (fst (EevdfClight.weighted_fractional_state_steps
          (Z.to_nat i) remainder weight 0)))
        by (change Int64.min_signed with (-9223372036854775808);
            change Int64.max_signed with 9223372036854775807;
            unfold c_int64_value, c_int64_min, c_int64_max in *;
            lia).
      change Int64.min_signed with (-9223372036854775808).
	      change Int64.max_signed with 9223372036854775807.
	      unfold c_int64_value, c_int64_min, c_int64_max in *.
	      lia.
	    + forward.
	      pose proof (weighted_fractional_state_steps_fractional_range_10
	        (Z.to_nat i) remainder weight) as Hfractional_range_then.
	      assert (Hfuel_then : (Z.to_nat i <= 10)%nat).
	      {
	        apply Nat2Z.inj_le.
	        rewrite Z2Nat.id by lia.
	        lia.
	      }
	      specialize (Hfractional_range_then Hfuel_then).
	      change (Int.signed (Int.repr 1)) with 1.
	      rewrite Int64.add_signed.
	      rewrite (Int64.signed_repr
	        (snd (EevdfClight.weighted_fractional_state_steps
	          (Z.to_nat i) remainder weight 0) * 2))
	        by (change Int64.min_signed with (-9223372036854775808);
	            change Int64.max_signed with 9223372036854775807;
	            lia).
	      rewrite (Int64.signed_repr 1)
	        by (change Int64.min_signed with (-9223372036854775808);
	            change Int64.max_signed with 9223372036854775807;
	            lia).
	      forward.
	      entailer!.
	      pose proof (weighted_fractional_state_steps_remainder_range
	        (Z.to_nat i) remainder weight 0) as Hremainder_current_then.
	      specialize (Hremainder_current_then ltac:(assumption)
	        ltac:(assumption)).
	      repeat rewrite Int64.signed_repr
	        by (change Int64.min_signed with (-9223372036854775808);
	            change Int64.max_signed with 9223372036854775807;
	            unfold c_int64_value, c_int64_min, c_int64_max in *;
	            lia).
	      change Int64.min_signed with (-9223372036854775808).
	      change Int64.max_signed with 9223372036854775807.
	      unfold c_int64_value, c_int64_min, c_int64_max in *.
	      split; lia.
	      entailer!.
	      assert (Hfuel_succ :
	        Z.to_nat (i + 1) = S (Z.to_nat i)).
	      {
	        replace (i + 1) with (Z.succ i) by lia.
	        rewrite Z2Nat.inj_succ by lia.
	        reflexivity.
	      }
	      rewrite Hfuel_succ.
	      rewrite weighted_fractional_state_steps_succ.
	      unfold EevdfClight.weighted_fractional_step_state.
	      pose proof (weighted_fractional_state_steps_remainder_range
	        (Z.to_nat i) remainder weight 0) as Hremainder_current_then2.
	      specialize (Hremainder_current_then2 ltac:(assumption)
	        ltac:(assumption)).
	      assert (Htake :
	        (weight -
	          fst (EevdfClight.weighted_fractional_state_steps
	            (Z.to_nat i) remainder weight 0)
	        <=?
	          fst (EevdfClight.weighted_fractional_state_steps
	            (Z.to_nat i) remainder weight 0)) = true).
	      {
	        apply Z.leb_le.
	        match goal with
	        | Hlt : Int64.lt _ _ = false |- _ =>
	            apply lt_false_inv64 in Hlt;
	            repeat rewrite Int64.signed_repr in Hlt by
	              (change Int64.min_signed with (-9223372036854775808);
	               change Int64.max_signed with 9223372036854775807;
	               unfold c_int64_value, c_int64_min, c_int64_max in *;
	               lia);
	            unfold c_int64_value, c_int64_min, c_int64_max in *;
	            lia
	        end.
	      }
	      rewrite Htake.
	      simpl.
	      split; reflexivity.
	    + forward.
	      entailer!.
	      pose proof (weighted_fractional_state_steps_remainder_range
	        (Z.to_nat i) remainder weight 0) as Hremainder_current_else.
	      specialize (Hremainder_current_else ltac:(assumption)
	        ltac:(assumption)).
	      match goal with
	      | Hlt : Int64.lt _ _ = true |- _ =>
	          apply lt_inv64 in Hlt;
	          repeat rewrite Int64.signed_repr in Hlt by
	            (change Int64.min_signed with (-9223372036854775808);
	             change Int64.max_signed with 9223372036854775807;
	             unfold c_int64_value, c_int64_min, c_int64_max in *;
	             lia)
	      end.
	      repeat rewrite Int64.signed_repr
	        by (change Int64.min_signed with (-9223372036854775808);
	            change Int64.max_signed with 9223372036854775807;
	            unfold c_int64_value, c_int64_min, c_int64_max in *;
	            lia).
	      change Int64.min_signed with (-9223372036854775808).
	      change Int64.max_signed with 9223372036854775807.
	      unfold c_int64_value, c_int64_min, c_int64_max in *.
	      lia.
	      pose proof (weighted_fractional_state_steps_remainder_range
	        (Z.to_nat i) remainder weight 0) as Hremainder_current_else_after.
	      specialize (Hremainder_current_else_after ltac:(assumption)
	        ltac:(assumption)).
	      rewrite Int64.add_signed.
	      rewrite (Int64.signed_repr
	        (fst (EevdfClight.weighted_fractional_state_steps
	          (Z.to_nat i) remainder weight 0)))
	        by (change Int64.min_signed with (-9223372036854775808);
	            change Int64.max_signed with 9223372036854775807;
	            unfold c_int64_value, c_int64_min, c_int64_max in *;
	            lia).
	      entailer!.
	      assert (Hfuel_succ :
	        Z.to_nat (i + 1) = S (Z.to_nat i)).
	      {
	        replace (i + 1) with (Z.succ i) by lia.
	        rewrite Z2Nat.inj_succ by lia.
	        reflexivity.
	      }
	      rewrite Hfuel_succ.
	      rewrite weighted_fractional_state_steps_succ.
	      unfold EevdfClight.weighted_fractional_step_state.
	      pose proof (weighted_fractional_state_steps_remainder_range
	        (Z.to_nat i) remainder weight 0) as Hremainder_current_else2.
	      specialize (Hremainder_current_else2 ltac:(assumption)
	        ltac:(assumption)).
	      assert (Htake :
	        (weight -
	          fst (EevdfClight.weighted_fractional_state_steps
	            (Z.to_nat i) remainder weight 0)
	        <=?
	          fst (EevdfClight.weighted_fractional_state_steps
	            (Z.to_nat i) remainder weight 0)) = false).
	      {
	        apply Z.leb_gt.
	        match goal with
	        | Hlt : Int64.lt _ _ = true |- _ =>
	            apply lt_inv64 in Hlt;
	            repeat rewrite Int64.signed_repr in Hlt by
	              (change Int64.min_signed with (-9223372036854775808);
	               change Int64.max_signed with 9223372036854775807;
	               unfold c_int64_value, c_int64_min, c_int64_max in *;
	               lia);
	            unfold c_int64_value, c_int64_min, c_int64_max in *;
	            lia
	        end.
	      }
	      rewrite Htake.
	      simpl.
	      split; reflexivity.
  - forward.
    change (Z.to_nat 10) with 10%nat in *.
    forward.
    entailer!.
    unfold c_int64_value, c_int64_min, c_int64_max.
    pose proof (weighted_fractional_state_steps_fractional_range_10
      10 remainder weight (le_n 10)) as Hfractional_range.
    lia.
Qed.

Lemma weighted_delta_ok_false_weight :
  forall runtime_ns weight,
    (0 <=? runtime_ns) = true ->
    (0 <? weight) = false ->
    EevdfClight.weighted_delta_ok runtime_ns weight = false.
Proof.
  intros runtime_ns weight Hruntime Hweight.
  unfold EevdfClight.weighted_delta_ok.
  rewrite Hruntime, Hweight.
  reflexivity.
Qed.

Lemma weighted_delta_ok_false_mul :
  forall runtime_ns weight,
    (0 <=? runtime_ns) = true ->
    (0 <? weight) = true ->
    EevdfClight.checked_mul_i64_nonnegative_ok
      (EevdfClight.weighted_delta_quotient runtime_ns weight) 1024 =
      false ->
    EevdfClight.weighted_delta_ok runtime_ns weight = false.
Proof.
  intros runtime_ns weight Hruntime Hweight Hmul.
  unfold EevdfClight.weighted_delta_ok.
  rewrite Hruntime, Hweight, Hmul.
  reflexivity.
Qed.

Lemma weighted_delta_ok_false_add :
  forall runtime_ns weight,
    (0 <=? runtime_ns) = true ->
    (0 <? weight) = true ->
    EevdfClight.checked_mul_i64_nonnegative_ok
      (EevdfClight.weighted_delta_quotient runtime_ns weight) 1024 =
      true ->
    EevdfClight.checked_add_i64_ok
      (EevdfClight.weighted_delta_whole runtime_ns weight)
      (EevdfClight.weighted_delta_fractional runtime_ns weight) =
      false ->
    EevdfClight.weighted_delta_ok runtime_ns weight = false.
Proof.
  intros runtime_ns weight Hruntime Hweight Hmul Hadd.
  unfold EevdfClight.weighted_delta_ok.
  rewrite Hruntime, Hweight, Hmul, Hadd.
  reflexivity.
Qed.

Lemma weighted_delta_quotient_range :
  forall runtime_ns weight,
    c_int64_value runtime_ns ->
    0 <= runtime_ns ->
    0 < weight ->
    c_int64_value
      (EevdfClight.weighted_delta_quotient runtime_ns weight).
Proof.
  intros runtime_ns weight Hruntime Hruntime_nonnegative Hweight.
  unfold EevdfClight.weighted_delta_quotient.
  unfold c_int64_value, c_int64_min, c_int64_max in *.
  assert (Hquot_nonnegative : 0 <= runtime_ns / weight)
    by (apply Z.div_pos; lia).
  assert (Hquot_le : runtime_ns / weight <= runtime_ns).
  {
    assert (Hmul_bound : (runtime_ns <= weight * runtime_ns)%Z)
      by nia.
    exact (Z.div_le_upper_bound runtime_ns weight runtime_ns
      Hweight Hmul_bound).
  }
  lia.
Qed.

Lemma weighted_delta_remainder_range :
  forall runtime_ns weight,
    0 <= runtime_ns ->
    0 < weight ->
    0 <= EevdfClight.weighted_delta_remainder runtime_ns weight < weight.
Proof.
  intros runtime_ns weight Hruntime Hweight.
  unfold EevdfClight.weighted_delta_remainder.
  apply Z.mod_pos_bound.
  lia.
Qed.

Lemma int64_repr_positive_not_zero :
  forall value,
    c_int64_value value ->
    0 < value ->
    Int64.repr value <> Int64.zero.
Proof.
  intros value Hrange Hpositive Heq.
  change Int64.zero with (Int64.repr 0) in Heq.
  apply (f_equal Int64.signed) in Heq.
  unfold c_int64_value, c_int64_min, c_int64_max in Hrange.
  rewrite Int64.signed_repr in Heq by
    (change Int64.min_signed with (-9223372036854775808);
     change Int64.max_signed with 9223372036854775807;
     lia).
  rewrite Int64.signed_repr in Heq by
    (change Int64.min_signed with (-9223372036854775808);
     change Int64.max_signed with 9223372036854775807;
     lia).
  lia.
Qed.

Lemma int64_signed_division_tc_positive_denominator :
  forall numerator denominator,
    c_int64_value numerator ->
    c_int64_value denominator ->
    0 < denominator ->
    Int64.repr denominator <> Int64.zero /\
      ~
        (Int64.repr numerator = Int64.repr Int64.min_signed /\
         Int64.repr denominator = Int64.mone).
Proof.
  intros numerator denominator Hnum Hden Hpositive.
  split.
  - apply int64_repr_positive_not_zero; assumption.
  - intros [_ Hden_mone].
    change Int64.mone with (Int64.repr (-1)) in Hden_mone.
    apply (f_equal Int64.signed) in Hden_mone.
    unfold c_int64_value, c_int64_min, c_int64_max in Hden.
    rewrite Int64.signed_repr in Hden_mone by
      (change Int64.min_signed with (-9223372036854775808);
       change Int64.max_signed with 9223372036854775807;
       lia).
    rewrite Int64.signed_repr in Hden_mone by
      (change Int64.min_signed with (-9223372036854775808);
       change Int64.max_signed with 9223372036854775807;
       lia).
    lia.
Qed.

Lemma int64_divs_repr_nonnegative :
  forall numerator denominator,
    c_int64_value numerator ->
    c_int64_value denominator ->
    0 <= numerator ->
    0 < denominator ->
    Int64.divs (Int64.repr numerator) (Int64.repr denominator) =
      Int64.repr (numerator / denominator).
Proof.
  intros numerator denominator Hnum Hden Hnum_nonnegative Hden_positive.
  unfold Int64.divs.
  unfold c_int64_value, c_int64_min, c_int64_max in *.
  rewrite Int64.signed_repr by
    (change Int64.min_signed with (-9223372036854775808);
     change Int64.max_signed with 9223372036854775807;
     lia).
  rewrite Int64.signed_repr by
    (change Int64.min_signed with (-9223372036854775808);
     change Int64.max_signed with 9223372036854775807;
     lia).
  rewrite Z.quot_div_nonneg by lia.
  reflexivity.
Qed.

Lemma int64_mods_repr_nonnegative :
  forall numerator denominator,
    c_int64_value numerator ->
    c_int64_value denominator ->
    0 <= numerator ->
    0 < denominator ->
    Int64.mods (Int64.repr numerator) (Int64.repr denominator) =
      Int64.repr (numerator mod denominator).
Proof.
  intros numerator denominator Hnum Hden Hnum_nonnegative Hden_positive.
  unfold Int64.mods.
  unfold c_int64_value, c_int64_min, c_int64_max in *.
  rewrite Int64.signed_repr by
    (change Int64.min_signed with (-9223372036854775808);
     change Int64.max_signed with 9223372036854775807;
     lia).
  rewrite Int64.signed_repr by
    (change Int64.min_signed with (-9223372036854775808);
     change Int64.max_signed with 9223372036854775807;
     lia).
  rewrite Z.rem_mod_nonneg by lia.
  reflexivity.
Qed.

Lemma weighted_slice_ok_true_range :
  forall slice_ns weight,
    EevdfClight.weighted_slice_ok slice_ns weight = true ->
    c_int64_value (EevdfClight.weighted_slice_value slice_ns weight).
Proof.
  intros slice_ns weight Hok.
  unfold EevdfClight.weighted_slice_value.
  pose proof (weighted_delta_ok_true_range slice_ns weight Hok)
    as Hdelta_range.
  unfold z_max.
  destruct (1 <? EevdfClight.weighted_delta_value slice_ns weight) eqn:Hlt.
  - exact Hdelta_range.
  - unfold c_int64_value, c_int64_min, c_int64_max.
    split; lia.
Qed.

Lemma body_checked_mul_i64_nonnegative :
  semax_body
    EevdfClight.Vprog
    EevdfClight.Gprog
    C.f_checked_mul_i64_nonnegative
    EevdfClight.checked_mul_i64_nonnegative_spec.
Proof.
  start_function.
  forward_call (lhs).
  destruct (0 <=? lhs) eqn:Hlhs_nonneg.
  - forward_if (
      PROP ()
      LOCAL (
        temp C._t'1 (EevdfClight.v_bool true);
        temp C._lhs (EevdfClight.v_i64 lhs);
        temp C._rhs (EevdfClight.v_i64 rhs);
        temp C._out out
      )
      SEP (data_at Tsh tlong (EevdfClight.v_i64 old_out) out)).
	    + exfalso.
	      match goal with
	      | Hbad : Int.zero <> Int.zero |- _ => exact (Hbad eq_refl)
	      | Hbad : Int.repr 0 <> Int.zero |- _ =>
	          try change (Int.repr 0) with Int.zero in Hbad;
	          exact (Hbad eq_refl)
	      | Hbad : Int.repr 1 = Int.zero |- _ =>
	          try change (Int.repr 1) with Int.one in Hbad;
	          exact (Int.one_not_zero Hbad)
	      end.
    + forward.
      entailer!.
    + forward_call (rhs).
      destruct (0 <=? rhs) eqn:Hrhs_nonneg.
      * forward_if (
          PROP ()
          LOCAL (
            temp C._t'2 (EevdfClight.v_bool true);
            temp C._t'1 (EevdfClight.v_bool true);
            temp C._lhs (EevdfClight.v_i64 lhs);
            temp C._rhs (EevdfClight.v_i64 rhs);
            temp C._out out
          )
          SEP (data_at Tsh tlong (EevdfClight.v_i64 old_out) out)).
	        -- exfalso.
	           match goal with
	           | Hbad : Int.zero <> Int.zero |- _ => exact (Hbad eq_refl)
	           | Hbad : Int.repr 0 <> Int.zero |- _ =>
	               try change (Int.repr 0) with Int.zero in Hbad;
	               exact (Hbad eq_refl)
	           | Hbad : Int.repr 1 = Int.zero |- _ =>
	               try change (Int.repr 1) with Int.one in Hbad;
	               exact (Int.one_not_zero Hbad)
	           end.
        -- forward.
           entailer!.
        -- forward_call (lhs, rhs).
           destruct
             (if rhs =? 0 then false else c_int64_max / rhs <? lhs)
             eqn:Hover.
           ++ forward_if.
              ** forward.
                 rewrite (checked_mul_i64_nonnegative_ok_false_overflow
                   lhs rhs Hlhs_nonneg Hrhs_nonneg Hover).
                 entailer!.
              ** exfalso.
                 match goal with
                 | Hbad : Int.repr 1 = Int.zero |- _ =>
                     try change (Int.repr 1) with Int.one in Hbad;
                     exact (Int.one_not_zero Hbad)
                 end.
           ++ forward_if (
                PROP ()
                LOCAL (
                  temp C._t'3 (EevdfClight.v_bool false);
                  temp C._t'2 (EevdfClight.v_bool true);
                  temp C._t'1 (EevdfClight.v_bool true);
                  temp C._lhs (EevdfClight.v_i64 lhs);
                  temp C._rhs (EevdfClight.v_i64 rhs);
                  temp C._out out
                )
                SEP (data_at Tsh tlong
                  (EevdfClight.v_i64 old_out) out)).
              ** exfalso.
                 match goal with
                 | Hbad : Int.zero <> Int.zero |- _ => exact (Hbad eq_refl)
                 | Hbad : Int.repr 0 <> Int.zero |- _ =>
                     try change (Int.repr 0) with Int.zero in Hbad;
                     exact (Hbad eq_refl)
                 end.
              ** forward.
                 entailer!.
              ** assert (Hmul_ok :
                   EevdfClight.checked_mul_i64_nonnegative_ok lhs rhs = true)
                   by (apply checked_mul_i64_nonnegative_ok_true_no_overflow;
                       assumption).
                 forward.
                 entailer!.
                 assert (Hlhs_range :
                   Int64.min_signed <= lhs <= Int64.max_signed)
                   by (change Int64.min_signed with (-9223372036854775808);
                       change Int64.max_signed with 9223372036854775807;
                       unfold c_int64_value, c_int64_min, c_int64_max in *;
                       lia).
                 assert (Hrhs_range :
                   Int64.min_signed <= rhs <= Int64.max_signed)
                   by (change Int64.min_signed with (-9223372036854775808);
                       change Int64.max_signed with 9223372036854775807;
                       unfold c_int64_value, c_int64_min, c_int64_max in *;
                       lia).
                 rewrite (Int64.signed_repr lhs) by exact Hlhs_range.
                 rewrite (Int64.signed_repr rhs) by exact Hrhs_range.
	                 apply checked_mul_i64_nonnegative_ok_true_range.
	                 exact Hmul_ok.
	                 forward.
	                 rewrite Hmul_ok.
	                 entailer!.
      * forward_if.
        -- forward.
           rewrite (checked_mul_i64_nonnegative_ok_false_rhs
             lhs rhs Hlhs_nonneg Hrhs_nonneg).
           entailer!.
        -- exfalso.
           match goal with
           | Hbad : Int.zero <> Int.zero |- _ => exact (Hbad eq_refl)
           | Hbad : Int.repr 0 <> Int.zero |- _ =>
               try change (Int.repr 0) with Int.zero in Hbad;
               exact (Hbad eq_refl)
           | Hbad : Int.repr 1 = Int.zero |- _ =>
               try change (Int.repr 1) with Int.one in Hbad;
               exact (Int.one_not_zero Hbad)
           end.
	  - forward_if.
	    + forward.
	      rewrite (checked_mul_i64_nonnegative_ok_false_lhs
	        lhs rhs Hlhs_nonneg).
      entailer!.
    + exfalso.
      match goal with
      | Hbad : Int.zero <> Int.zero |- _ => exact (Hbad eq_refl)
      | Hbad : Int.repr 0 <> Int.zero |- _ =>
          try change (Int.repr 0) with Int.zero in Hbad;
          exact (Hbad eq_refl)
      | Hbad : Int.repr 1 = Int.zero |- _ =>
          try change (Int.repr 1) with Int.one in Hbad;
          exact (Int.one_not_zero Hbad)
	      end.
Qed.

Lemma body_weighted_delta :
  semax_body
    EevdfClight.Vprog
    EevdfClight.Gprog
    C.f_weighted_delta
    EevdfClight.weighted_delta_spec.
Proof.
  start_function.
  forward_call (runtime_ns).
  destruct (0 <=? runtime_ns) eqn:Hruntime_nonnegative.
  - forward_if (
      PROP ()
      LOCAL (
        lvar C._whole tlong v_whole;
        lvar C._fractional tlong v_fractional;
        lvar C._delta tlong v_delta;
        temp C._t'1 (EevdfClight.v_bool true);
        temp C._runtime_ns (EevdfClight.v_i64 runtime_ns);
        temp C._weight (EevdfClight.v_i64 weight);
        temp C._out out
      )
      SEP (
        data_at_ Tsh tlong v_whole;
        data_at_ Tsh tlong v_fractional;
        data_at_ Tsh tlong v_delta;
        data_at_ Tsh tlong out
      )).
    + exfalso.
      match goal with
      | Hbad : Int.repr 1 = Int.zero |- _ =>
          try change (Int.repr 1) with Int.one in Hbad;
          exact (Int.one_not_zero Hbad)
      | Hbad : Int.one = Int.zero |- _ =>
          exact (Int.one_not_zero Hbad)
      end.
    + forward.
      entailer!.
    + forward_call (weight).
      destruct (0 <? weight) eqn:Hweight_positive.
      * forward_if (
          PROP ()
          LOCAL (
            lvar C._whole tlong v_whole;
            lvar C._fractional tlong v_fractional;
            lvar C._delta tlong v_delta;
            temp C._t'2 (EevdfClight.v_bool true);
            temp C._t'1 (EevdfClight.v_bool true);
            temp C._runtime_ns (EevdfClight.v_i64 runtime_ns);
            temp C._weight (EevdfClight.v_i64 weight);
            temp C._out out
          )
          SEP (
            data_at_ Tsh tlong v_whole;
            data_at_ Tsh tlong v_fractional;
            data_at_ Tsh tlong v_delta;
            data_at_ Tsh tlong out
          )).
        -- exfalso.
           match goal with
           | Hbad : Int.repr 1 = Int.zero |- _ =>
               try change (Int.repr 1) with Int.one in Hbad;
               exact (Int.one_not_zero Hbad)
           | Hbad : Int.one = Int.zero |- _ =>
               exact (Int.one_not_zero Hbad)
           end.
        -- forward.
           entailer!.
        -- assert (Hruntime_nonnegative_prop : 0 <= runtime_ns)
             by (apply Z.leb_le; exact Hruntime_nonnegative).
           assert (Hweight_positive_prop : 0 < weight)
             by (apply Z.ltb_lt; exact Hweight_positive).
           assert (Hquotient_range :
             c_int64_value
               (EevdfClight.weighted_delta_quotient runtime_ns weight))
             by (apply weighted_delta_quotient_range; assumption).
           assert (Hremainder_range :
             0 <= EevdfClight.weighted_delta_remainder runtime_ns weight <
               weight)
             by (apply weighted_delta_remainder_range; assumption).
           forward.
           entailer!.
           apply int64_signed_division_tc_positive_denominator;
             assumption.
           forward.
           entailer!.
           apply int64_signed_division_tc_positive_denominator;
             assumption.
           forward.
           forward_call
             (EevdfClight.weighted_delta_quotient runtime_ns weight,
              1024, v_whole, 0).
           {
             unfold EevdfClight.weighted_delta_quotient in *.
             entailer!;
               try (rewrite (int64_divs_repr_nonnegative runtime_ns weight)
                 by assumption; reflexivity);
               try (split;
                 unfold c_int64_value, c_int64_min, c_int64_max;
                 split; lia).
           }
           {
             split;
               unfold c_int64_value, c_int64_min, c_int64_max;
               split; lia.
           }
           destruct
             (EevdfClight.checked_mul_i64_nonnegative_ok
               (EevdfClight.weighted_delta_quotient runtime_ns weight)
               1024)
             eqn:Hmul_ok.
           ++ forward_if (
                PROP ()
                LOCAL (
                  lvar C._whole tlong v_whole;
                  lvar C._fractional tlong v_fractional;
                  lvar C._delta tlong v_delta;
                  temp C._t'3 (EevdfClight.v_bool true);
                  temp C._remainder
                    (EevdfClight.v_i64
                      (EevdfClight.weighted_delta_remainder
                        runtime_ns weight));
                  temp C._quotient
                    (EevdfClight.v_i64
                      (EevdfClight.weighted_delta_quotient
                        runtime_ns weight));
                  temp C._t'2 (EevdfClight.v_bool true);
                  temp C._t'1 (EevdfClight.v_bool true);
                  temp C._runtime_ns (EevdfClight.v_i64 runtime_ns);
                  temp C._weight (EevdfClight.v_i64 weight);
                  temp C._out out
                )
                SEP (
                  data_at Tsh tlong
                    (EevdfClight.v_i64
                      (EevdfClight.weighted_delta_whole
                        runtime_ns weight)) v_whole;
                  data_at_ Tsh tlong v_fractional;
                  data_at_ Tsh tlong v_delta;
                  data_at_ Tsh tlong out
                )).
              ** exfalso.
                 match goal with
                 | Hbad : Int.repr 1 = Int.zero |- _ =>
                     try change (Int.repr 1) with Int.one in Hbad;
                     exact (Int.one_not_zero Hbad)
                 | Hbad : Int.one = Int.zero |- _ =>
                     exact (Int.one_not_zero Hbad)
                 end.
              ** forward.
                 entailer!.
                 split;
                 [ unfold EevdfClight.weighted_delta_remainder,
                     EevdfClight.v_i64;
                   rewrite (int64_mods_repr_nonnegative
                     runtime_ns weight) by assumption;
                   reflexivity
                 | unfold EevdfClight.weighted_delta_quotient,
                     EevdfClight.v_i64;
                   rewrite (int64_divs_repr_nonnegative
                     runtime_ns weight) by assumption;
                   reflexivity ].
              ** forward.
                 forward_call
                   (EevdfClight.weighted_delta_remainder runtime_ns weight,
                    weight, v_fractional).
                 {
                   unfold EevdfClight.weighted_delta_remainder in *.
                   unfold c_int64_value, c_int64_min, c_int64_max in *.
                   lia.
                 }
                 change
                   (snd
                     (EevdfClight.weighted_fractional_state_steps 10
                       (EevdfClight.weighted_delta_remainder
                         runtime_ns weight) weight 0))
                   with
                     (EevdfClight.weighted_delta_fractional
                       runtime_ns weight) in *.
                 forward_if (
                   PROP (
                     c_int64_value
                       (EevdfClight.weighted_delta_fractional
                         runtime_ns weight)
                   )
                   LOCAL (
                     temp C._t'4 (EevdfClight.v_bool true);
                     lvar C._whole tlong v_whole;
                     lvar C._fractional tlong v_fractional;
                     lvar C._delta tlong v_delta;
                     temp C._t'3 (EevdfClight.v_bool true);
                     temp C._remainder
                       (EevdfClight.v_i64
                         (EevdfClight.weighted_delta_remainder
                           runtime_ns weight));
                     temp C._quotient
                       (EevdfClight.v_i64
                         (EevdfClight.weighted_delta_quotient
                           runtime_ns weight));
                     temp C._t'2 (EevdfClight.v_bool true);
                     temp C._t'1 (EevdfClight.v_bool true);
                     temp C._runtime_ns (EevdfClight.v_i64 runtime_ns);
                     temp C._weight (EevdfClight.v_i64 weight);
                     temp C._out out
                   )
                   SEP (
                     data_at Tsh tlong
                       (EevdfClight.v_i64
                         (EevdfClight.weighted_delta_fractional
                           runtime_ns weight)) v_fractional;
                     data_at Tsh tlong
                       (EevdfClight.v_i64
                         (EevdfClight.weighted_delta_whole
                           runtime_ns weight)) v_whole;
                     data_at_ Tsh tlong v_delta;
                     data_at_ Tsh tlong out
                   )).
                 --- exfalso.
                    match goal with
                    | Hbad : Int.repr 1 = Int.zero |- _ =>
                        try change (Int.repr 1) with Int.one in Hbad;
                        exact (Int.one_not_zero Hbad)
                    | Hbad : Int.one = Int.zero |- _ =>
                        exact (Int.one_not_zero Hbad)
                    end.
                 --- forward.
                    entailer!.
                 --- forward.
                    assert (Hwhole_range :
                      c_int64_value
                        (EevdfClight.weighted_delta_whole
                          runtime_ns weight)).
                    {
                      unfold EevdfClight.weighted_delta_whole.
                      apply checked_mul_i64_nonnegative_ok_true_range.
                      exact Hmul_ok.
                    }
                    assert (Hfractional_range :
                      c_int64_value
                        (EevdfClight.weighted_delta_fractional
                          runtime_ns weight)).
                    {
                      unfold EevdfClight.weighted_delta_fractional.
                      unfold c_int64_value, c_int64_min, c_int64_max.
                      pose proof
                        (weighted_fractional_state_steps_fractional_range_10
                          10
                          (EevdfClight.weighted_delta_remainder
                            runtime_ns weight)
                          weight (le_n 10)) as Hfrac_range.
                      lia.
                    }
                    forward.
                    forward.
	                    forward_call
	                      (EevdfClight.weighted_delta_whole
	                        runtime_ns weight,
	                       EevdfClight.weighted_delta_fractional
	                        runtime_ns weight,
	                       v_delta,
	                       0).
	                    { unfold c_int64_value, c_int64_min, c_int64_max;
	                      split; lia. }
                    destruct
                      (EevdfClight.checked_add_i64_ok
                        (EevdfClight.weighted_delta_whole
                          runtime_ns weight)
                        (EevdfClight.weighted_delta_fractional
                          runtime_ns weight))
                      eqn:Hadd_ok.
                    ++++ forward_if.
	                        **** exfalso.
	                            match goal with
	                            | Hbad : Int.repr 1 = Int.zero |- _ =>
	                                try change (Int.repr 1) with Int.one in Hbad;
	                                exact (Int.one_not_zero Hbad)
	                            | Hbad : Int.one = Int.zero |- _ =>
	                                exact (Int.one_not_zero Hbad)
	                            end.
	                        **** forward.
	                            pose proof
	                              (checked_add_i64_ok_true_range _ _
	                                Hadd_ok) as Hadd_range.
	                            forward.
	                            unfold EevdfClight.weighted_delta_value.
	                            forward.
	                            unfold EevdfClight.weighted_delta_ok.
	                            rewrite Hruntime_nonnegative, Hweight_positive,
	                              Hmul_ok, Hadd_ok.
	                            entailer!.
                    ++++ forward_if.
                        **** forward.
                            rewrite (weighted_delta_ok_false_add
                              runtime_ns weight Hruntime_nonnegative
                              Hweight_positive Hmul_ok Hadd_ok).
                            entailer!.
                        **** exfalso.
                            match goal with
                            | Hbad : Int.zero <> Int.zero |- _ =>
                                exact (Hbad eq_refl)
                            | Hbad : Int.repr 0 <> Int.zero |- _ =>
                                try change (Int.repr 0) with Int.zero in Hbad;
                                exact (Hbad eq_refl)
                            | Hbad : Int.repr 1 = Int.zero |- _ =>
                                try change (Int.repr 1) with Int.one in Hbad;
                                exact (Int.one_not_zero Hbad)
                            end.
           ++ forward_if.
              ** forward.
                 rewrite (weighted_delta_ok_false_mul
                   runtime_ns weight Hruntime_nonnegative
                   Hweight_positive Hmul_ok).
                 entailer!.
              ** exfalso.
                 match goal with
                 | Hbad : Int.zero <> Int.zero |- _ =>
                     exact (Hbad eq_refl)
                 | Hbad : Int.repr 0 <> Int.zero |- _ =>
                     try change (Int.repr 0) with Int.zero in Hbad;
                     exact (Hbad eq_refl)
                 end.
      * forward_if.
        -- forward.
           rewrite (weighted_delta_ok_false_weight
             runtime_ns weight Hruntime_nonnegative Hweight_positive).
           entailer!.
        -- exfalso.
           match goal with
           | Hbad : Int.zero <> Int.zero |- _ => exact (Hbad eq_refl)
           | Hbad : Int.repr 0 <> Int.zero |- _ =>
               try change (Int.repr 0) with Int.zero in Hbad;
               exact (Hbad eq_refl)
           end.
  - forward_if.
    + forward.
      rewrite (weighted_delta_ok_false_runtime
        runtime_ns weight Hruntime_nonnegative).
      entailer!.
    + exfalso.
      match goal with
      | Hbad : Int.zero <> Int.zero |- _ => exact (Hbad eq_refl)
      | Hbad : Int.repr 0 <> Int.zero |- _ =>
          try change (Int.repr 0) with Int.zero in Hbad;
          exact (Hbad eq_refl)
      end.
Qed.

Lemma body_weighted_slice :
  semax_body
    EevdfClight.Vprog
    EevdfClight.Gprog
    C.f_weighted_slice
    EevdfClight.weighted_slice_spec.
Proof.
  start_function.
  forward_call (slice_ns, weight, v_delta).
  destruct (EevdfClight.weighted_delta_ok slice_ns weight) eqn:Hdelta_ok.
  - forward_if (
      PROP ()
      LOCAL (
        lvar C._delta tlong v_delta;
        temp C._t'1 (EevdfClight.v_bool true);
        temp C._slice_ns (EevdfClight.v_i64 slice_ns);
        temp C._weight (EevdfClight.v_i64 weight);
        temp C._out out
      )
      SEP (
        data_at Tsh tlong
            (EevdfClight.v_i64
            (EevdfClight.weighted_delta_value slice_ns weight)) v_delta;
        data_at Tsh tlong (EevdfClight.v_i64 old_out) out
      )).
    + exfalso.
      match goal with
      | Hbad : Int.repr 0 <> Int.zero |- _ =>
          try change (Int.repr 0) with Int.zero in Hbad;
          exact (Hbad eq_refl)
      | Hbad : Int.zero <> Int.zero |- _ => exact (Hbad eq_refl)
      | Hbad : Int.repr 1 = Int.zero |- _ =>
          try change (Int.repr 1) with Int.one in Hbad;
          exact (Int.one_not_zero Hbad)
      | Hbad : Int.one = Int.zero |- _ =>
          exact (Int.one_not_zero Hbad)
      | Hbad : Int.eq (Int.repr 1) Int.zero = true |- _ =>
          discriminate Hbad
      | Hbad : Int.eq Int.one Int.zero = true |- _ =>
          discriminate Hbad
      end.
    + forward.
      entailer!.
    + forward.
      forward_call
        (1, EevdfClight.weighted_delta_value slice_ns weight).
      * split.
        -- unfold c_int64_value, c_int64_min, c_int64_max.
           lia.
        -- apply weighted_delta_ok_true_range.
           exact Hdelta_ok.
      * forward.
        forward.
        unfold EevdfClight.weighted_slice_ok.
        rewrite Hdelta_ok.
        entailer!.
        apply weighted_slice_ok_true_range.
        exact Hdelta_ok.
  - forward_if.
    + forward.
      unfold EevdfClight.weighted_slice_ok.
      rewrite Hdelta_ok.
      entailer!.
    + exfalso.
      match goal with
      | Hbad : Int.repr 0 <> Int.zero |- _ =>
          try change (Int.repr 0) with Int.zero in Hbad;
          exact (Hbad eq_refl)
      | Hbad : Int.zero <> Int.zero |- _ => exact (Hbad eq_refl)
      | Hbad : Int.repr 1 = Int.zero |- _ =>
          try change (Int.repr 1) with Int.one in Hbad;
          exact (Int.one_not_zero Hbad)
      | Hbad : Int.one = Int.zero |- _ =>
          exact (Int.one_not_zero Hbad)
      | Hbad : Int.eq (Int.repr 0) Int.zero = false |- _ =>
          discriminate Hbad
      | Hbad : Int.eq Int.zero Int.zero = false |- _ =>
          discriminate Hbad
      end.
Qed.

Lemma body_entity_better_values :
  semax_body
    EevdfClight.Vprog
    EevdfClight.Gprog
    C.f_entity_better_values
    EevdfClight.entity_better_values_spec.
Proof.
  start_function.
  forward.
  forward_call (candidate_deadline, current_deadline).
  destruct (candidate_deadline <? current_deadline) eqn:Hdeadline_lt.
  - forward_if (
      PROP ()
      LOCAL (
        temp C._result (EevdfClight.v_bool true);
        temp C._t'3 (EevdfClight.v_bool true);
        temp C._candidate_deadline (EevdfClight.v_i64 candidate_deadline);
        temp C._current_deadline (EevdfClight.v_i64 current_deadline);
        temp C._candidate_thread_id (EevdfClight.v_i64 candidate_thread_id);
        temp C._current_thread_id (EevdfClight.v_i64 current_thread_id)
      )
      SEP ()).
    + forward.
      entailer!.
    + exfalso.
      match goal with
      | Hbad : Int.repr 1 = Int.zero |- _ =>
          try change (Int.repr 1) with Int.one in Hbad;
          exact (Int.one_not_zero Hbad)
      end.
    + forward.
      unfold EevdfClight.v_bool, EevdfClight.v_int,
        EevdfClight.entity_better_values.
      rewrite Hdeadline_lt.
      entailer!.
  - forward_if (
      PROP ()
      LOCAL (
        temp C._result
          (EevdfClight.v_bool
            (andb (candidate_deadline =? current_deadline)
              (candidate_thread_id <? current_thread_id)));
        temp C._t'3 (EevdfClight.v_bool false);
        temp C._candidate_deadline (EevdfClight.v_i64 candidate_deadline);
        temp C._current_deadline (EevdfClight.v_i64 current_deadline);
        temp C._candidate_thread_id (EevdfClight.v_i64 candidate_thread_id);
        temp C._current_thread_id (EevdfClight.v_i64 current_thread_id)
      )
      SEP ()).
    + exfalso.
      match goal with
      | Hbad : Int.zero <> Int.zero |- _ => exact (Hbad eq_refl)
      | Hbad : Int.repr 0 <> Int.zero |- _ =>
          try change (Int.repr 0) with Int.zero in Hbad;
          exact (Hbad eq_refl)
      end.
    + forward_call (candidate_deadline, current_deadline).
      destruct (candidate_deadline =? current_deadline) eqn:Hdeadline_eq.
      * forward_if (
          PROP ()
          LOCAL (
            temp C._result
              (EevdfClight.v_bool
                (candidate_thread_id <? current_thread_id));
            temp C._t'2 (EevdfClight.v_bool true);
            temp C._t'3 (EevdfClight.v_bool false);
            temp C._candidate_deadline
              (EevdfClight.v_i64 candidate_deadline);
            temp C._current_deadline (EevdfClight.v_i64 current_deadline);
            temp C._candidate_thread_id
              (EevdfClight.v_i64 candidate_thread_id);
            temp C._current_thread_id (EevdfClight.v_i64 current_thread_id)
          )
          SEP ()).
        -- forward_call (candidate_thread_id, current_thread_id).
           forward.
           entailer!.
        -- exfalso.
           match goal with
           | Hbad : Int.repr 1 = Int.zero |- _ =>
               try change (Int.repr 1) with Int.one in Hbad;
               exact (Int.one_not_zero Hbad)
           end.
        -- entailer!.
      * forward_if (
          PROP ()
          LOCAL (
            temp C._result (EevdfClight.v_bool false);
            temp C._t'2 (EevdfClight.v_bool false);
            temp C._t'3 (EevdfClight.v_bool false);
            temp C._candidate_deadline
              (EevdfClight.v_i64 candidate_deadline);
            temp C._current_deadline (EevdfClight.v_i64 current_deadline);
            temp C._candidate_thread_id
              (EevdfClight.v_i64 candidate_thread_id);
            temp C._current_thread_id (EevdfClight.v_i64 current_thread_id)
          )
          SEP ()).
        -- exfalso.
           match goal with
           | Hbad : Int.zero <> Int.zero |- _ => exact (Hbad eq_refl)
           | Hbad : Int.repr 0 <> Int.zero |- _ =>
               try change (Int.repr 0) with Int.zero in Hbad;
               exact (Hbad eq_refl)
           end.
        -- forward.
           entailer!.
        -- entailer!.
    + forward.
      entailer!.
      unfold EevdfClight.v_bool, EevdfClight.v_int,
        EevdfClight.entity_better_values.
      rewrite Hdeadline_lt.
      reflexivity.
Qed.

Lemma body_entity_better :
  semax_body
    EevdfClight.Vprog
    EevdfClight.Gprog
    C.f_entity_better
    EevdfClight.entity_better_spec.
Proof.
  start_function.
  destruct candidate as
    [candidate_thread_id candidate_generation candidate_weight
     candidate_slice_ns candidate_service_ns candidate_vruntime
     candidate_eligible_time candidate_deadline candidate_state].
  destruct current as
    [current_thread_id current_generation current_weight current_slice_ns
     current_service_ns current_vruntime current_eligible_time
     current_deadline current_state].
  forward.
  forward.
  forward.
  forward.
  forward_call
    (candidate_deadline, current_deadline,
     candidate_thread_id, current_thread_id).
  - unfold eevdf_entity_c_shape in *.
    intuition.
  - forward.
    forward.
Qed.

Lemma body_pacha_eevdf_empty_entity :
  semax_body
    EevdfClight.Vprog
    EevdfClight.Gprog
    C.f_pacha_eevdf_empty_entity
    EevdfClight.pacha_eevdf_empty_entity_spec.
Proof.
  start_function.
  forward.
  forward.
  forward.
  forward.
  forward.
  forward.
  forward.
  forward.
  forward.
  forward.
Qed.

Lemma body_pacha_eevdf_empty_runqueue :
  semax_body
    EevdfClight.Vprog
    EevdfClight.Gprog
    C.f_pacha_eevdf_empty_runqueue
    EevdfClight.pacha_eevdf_empty_runqueue_spec.
Proof.
  start_function.
  forward_for_simple_bound 256
    (EX i : Z,
      PROP ()
      LOCAL (temp C._out out)
      SEP (eevdf_empty_runqueue_prefix_rep Tsh i out)).
  - unfold eevdf_empty_runqueue_prefix_rep.
    rewrite eevdf_empty_runqueue_prefix_data_0.
    entailer!.
  - unfold eevdf_empty_runqueue_prefix_rep.
    unfold_data_at (data_at Tsh EevdfClight.t_eevdf_runqueue
      (eevdf_empty_runqueue_prefix_data i) out).
    change (fst (eevdf_empty_runqueue_prefix_data i))
      with (eevdf_empty_runqueue_prefix_entities i).
    assert (Hidx : (Z.to_nat i < eevdf_max_entities)%nat).
    {
      apply Nat2Z.inj_lt.
      rewrite Z2Nat.id by lia.
      change (Z.of_nat eevdf_max_entities) with 256.
      lia.
    }
    assert (Hlen :
      length (eevdf_empty_runqueue_prefix_entities i) =
      eevdf_max_entities).
    {
      apply eevdf_empty_runqueue_prefix_entities_length.
      lia.
    }
    assert (Hlookup :
      nth_error (eevdf_empty_runqueue_prefix_entities i)
        (Z.to_nat i) =
      Some (default_val EevdfClight.t_eevdf_entity)).
    {
      apply eevdf_empty_runqueue_prefix_entities_default_lookup.
      lia.
    }
    assert_PROP (field_compatible EevdfClight.t_eevdf_runqueue
      [StructField C._entities] out) as Hentities by entailer!.
    assert (Harray : field_compatible
      (Tarray EevdfClight.t_eevdf_entity 256 noattr)
      [ArraySubsc i]
      (field_address EevdfClight.t_eevdf_runqueue
        [StructField C._entities] out)).
    {
      change (Tarray EevdfClight.t_eevdf_entity 256 noattr)
        with (nested_field_type EevdfClight.t_eevdf_runqueue
          [StructField C._entities]).
      apply field_compatible_app.
      eapply field_compatible_cons_Tarray
        with (gfs := [StructField C._entities]).
      - reflexivity.
      - exact Hentities.
      - lia.
    }
    rewrite field_compatible_field_address in Harray by exact Hentities.
    change (nested_field_offset EevdfClight.t_eevdf_runqueue
      [StructField C._entities]) with 0 in Harray.
    sep_apply (eevdf_entity_array_value_ramif Tsh
      (eevdf_empty_runqueue_prefix_entities i)
      (default_val EevdfClight.t_eevdf_entity)
      (EevdfClight.eevdf_entity_data eevdf_empty_entity)
      out (Z.to_nat i) Hlen Hlookup Hidx).
    forward_call
      (eevdf_entity_array_data_element_ptr out (Z.to_nat i)).
    entailer!.
    unfold eevdf_entity_array_data_element_ptr.
    simpl.
    f_equal.
    rewrite Z2Nat.id by lia.
    unfold field_address.
    simpl.
    destruct (field_compatible_dec EevdfClight.t_eevdf_runqueue
      [StructField C._entities] out) as [Hentities_dec | Hentities_bad].
    2: contradiction.
    destruct (field_compatible_dec
      (Tarray EevdfClight.t_eevdf_entity 256 noattr)
      [ArraySubsc i] (offset_val 0 out)) as [Harray_dec | Harray_bad].
    2: contradiction.
    rewrite isptr_offset_val_zero
      by (eapply field_compatible_isptr; exact Hentities_dec).
    rewrite Z.add_0_l.
    reflexivity.
    rewrite data_at__eq.
    cancel.
    unfold EevdfClight.eevdf_entity_data_rep.
    sep_apply (wand_frame_elim
      (data_at Tsh EevdfClight.t_eevdf_entity
        (EevdfClight.eevdf_entity_data eevdf_empty_entity)
        (eevdf_entity_array_data_element_ptr out (Z.to_nat i)))
      (field_at Tsh EevdfClight.t_eevdf_runqueue
        [StructField C._entities]
        (ProtocolModel.replace_nth
          (eevdf_empty_runqueue_prefix_entities i)
          (Z.to_nat i)
          (EevdfClight.eevdf_entity_data eevdf_empty_entity))
        out)).
    rewrite eevdf_empty_runqueue_prefix_entities_step by lia.
    unfold eevdf_empty_runqueue_prefix_rep,
      eevdf_empty_runqueue_prefix_data.
    entailer!.
    change (eevdf_empty_runqueue_prefix_entities (i + 1),
      (default_val tulong,
       (default_val tulong, (default_val tlong, default_val tlong))))
      with (eevdf_empty_runqueue_prefix_data (i + 1)).
    unfold_data_at (data_at Tsh EevdfClight.t_eevdf_runqueue
      (eevdf_empty_runqueue_prefix_data (i + 1)) out).
    cancel.
  - unfold eevdf_empty_runqueue_prefix_rep,
      eevdf_empty_runqueue_prefix_data.
    rewrite eevdf_empty_runqueue_prefix_entities_256.
    forward.
    forward.
    forward.
    forward.
    forward.
Qed.

Lemma body_pacha_eevdf_reset :
  semax_body
    EevdfClight.Vprog
    EevdfClight.Gprog
    C.f_pacha_eevdf_reset
    EevdfClight.pacha_eevdf_reset_spec.
Proof.
  start_function.
  forward_call (out).
  forward.
  cancel.
Qed.
