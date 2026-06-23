From Coq Require Import String List ZArith.
From compcert Require Import Coqlib Integers Floats AST Ctypes Cop Clight Clightdefs.
Import Clightdefs.ClightNotations.
Local Open Scope Z_scope.
Local Open Scope string_scope.
Local Open Scope clight_scope.

Module Info.
  Definition version := "3.17".
  Definition build_number := "".
  Definition build_tag := "".
  Definition build_branch := "".
  Definition arch := "x86".
  Definition model := "64".
  Definition abi := "standard".
  Definition bitsize := 64.
  Definition big_endian := false.
  Definition source_file := "/home/kamer/os/verified/scheduling/src/pacha_eevdf.c".
  Definition normalized := true.
End Info.

Definition ___builtin_ais_annot : ident := $"__builtin_ais_annot".
Definition ___builtin_annot : ident := $"__builtin_annot".
Definition ___builtin_annot_intval : ident := $"__builtin_annot_intval".
Definition ___builtin_bswap : ident := $"__builtin_bswap".
Definition ___builtin_bswap16 : ident := $"__builtin_bswap16".
Definition ___builtin_bswap32 : ident := $"__builtin_bswap32".
Definition ___builtin_bswap64 : ident := $"__builtin_bswap64".
Definition ___builtin_clz : ident := $"__builtin_clz".
Definition ___builtin_clzl : ident := $"__builtin_clzl".
Definition ___builtin_clzll : ident := $"__builtin_clzll".
Definition ___builtin_ctz : ident := $"__builtin_ctz".
Definition ___builtin_ctzl : ident := $"__builtin_ctzl".
Definition ___builtin_ctzll : ident := $"__builtin_ctzll".
Definition ___builtin_debug : ident := $"__builtin_debug".
Definition ___builtin_expect : ident := $"__builtin_expect".
Definition ___builtin_fabs : ident := $"__builtin_fabs".
Definition ___builtin_fabsf : ident := $"__builtin_fabsf".
Definition ___builtin_fmadd : ident := $"__builtin_fmadd".
Definition ___builtin_fmax : ident := $"__builtin_fmax".
Definition ___builtin_fmin : ident := $"__builtin_fmin".
Definition ___builtin_fmsub : ident := $"__builtin_fmsub".
Definition ___builtin_fnmadd : ident := $"__builtin_fnmadd".
Definition ___builtin_fnmsub : ident := $"__builtin_fnmsub".
Definition ___builtin_fsqrt : ident := $"__builtin_fsqrt".
Definition ___builtin_membar : ident := $"__builtin_membar".
Definition ___builtin_memcpy_aligned : ident := $"__builtin_memcpy_aligned".
Definition ___builtin_read16_reversed : ident := $"__builtin_read16_reversed".
Definition ___builtin_read32_reversed : ident := $"__builtin_read32_reversed".
Definition ___builtin_sel : ident := $"__builtin_sel".
Definition ___builtin_sqrt : ident := $"__builtin_sqrt".
Definition ___builtin_unreachable : ident := $"__builtin_unreachable".
Definition ___builtin_va_arg : ident := $"__builtin_va_arg".
Definition ___builtin_va_copy : ident := $"__builtin_va_copy".
Definition ___builtin_va_end : ident := $"__builtin_va_end".
Definition ___builtin_va_start : ident := $"__builtin_va_start".
Definition ___builtin_write16_reversed : ident := $"__builtin_write16_reversed".
Definition ___builtin_write32_reversed : ident := $"__builtin_write32_reversed".
Definition ___compcert_i64_dtos : ident := $"__compcert_i64_dtos".
Definition ___compcert_i64_dtou : ident := $"__compcert_i64_dtou".
Definition ___compcert_i64_sar : ident := $"__compcert_i64_sar".
Definition ___compcert_i64_sdiv : ident := $"__compcert_i64_sdiv".
Definition ___compcert_i64_shl : ident := $"__compcert_i64_shl".
Definition ___compcert_i64_shr : ident := $"__compcert_i64_shr".
Definition ___compcert_i64_smod : ident := $"__compcert_i64_smod".
Definition ___compcert_i64_smulh : ident := $"__compcert_i64_smulh".
Definition ___compcert_i64_stod : ident := $"__compcert_i64_stod".
Definition ___compcert_i64_stof : ident := $"__compcert_i64_stof".
Definition ___compcert_i64_udiv : ident := $"__compcert_i64_udiv".
Definition ___compcert_i64_umod : ident := $"__compcert_i64_umod".
Definition ___compcert_i64_umulh : ident := $"__compcert_i64_umulh".
Definition ___compcert_i64_utod : ident := $"__compcert_i64_utod".
Definition ___compcert_i64_utof : ident := $"__compcert_i64_utof".
Definition ___compcert_va_composite : ident := $"__compcert_va_composite".
Definition ___compcert_va_float64 : ident := $"__compcert_va_float64".
Definition ___compcert_va_int32 : ident := $"__compcert_va_int32".
Definition ___compcert_va_int64 : ident := $"__compcert_va_int64".
Definition _best_index : ident := $"best_index".
Definition _bit : ident := $"bit".
Definition _candidate : ident := $"candidate".
Definition _candidate_deadline : ident := $"candidate_deadline".
Definition _candidate_thread_id : ident := $"candidate_thread_id".
Definition _checked_add_i64 : ident := $"checked_add_i64".
Definition _checked_add_i64_overflows : ident := $"checked_add_i64_overflows".
Definition _checked_mul_i64_nonnegative : ident := $"checked_mul_i64_nonnegative".
Definition _checked_mul_i64_nonnegative_overflows : ident := $"checked_mul_i64_nonnegative_overflows".
Definition _count : ident := $"count".
Definition _count_runnable : ident := $"count_runnable".
Definition _current : ident := $"current".
Definition _current_deadline : ident := $"current_deadline".
Definition _current_thread_id : ident := $"current_thread_id".
Definition _deadline : ident := $"deadline".
Definition _delta : ident := $"delta".
Definition _dst : ident := $"dst".
Definition _eligible : ident := $"eligible".
Definition _eligible_time : ident := $"eligible_time".
Definition _entities : ident := $"entities".
Definition _entity : ident := $"entity".
Definition _entity__1 : ident := $"entity__1".
Definition _entity__2 : ident := $"entity__2".
Definition _entity_better : ident := $"entity_better".
Definition _entity_better_values : ident := $"entity_better_values".
Definition _entity_count : ident := $"entity_count".
Definition _existing : ident := $"existing".
Definition _fail_runqueue : ident := $"fail_runqueue".
Definition _find_entity_index : ident := $"find_entity_index".
Definition _floor_vruntime : ident := $"floor_vruntime".
Definition _found : ident := $"found".
Definition _fractional : ident := $"fractional".
Definition _generation : ident := $"generation".
Definition _has_entity : ident := $"has_entity".
Definition _have_best : ident := $"have_best".
Definition _have_next : ident := $"have_next".
Definition _i : ident := $"i".
Definition _i64_equal : ident := $"i64_equal".
Definition _i64_less : ident := $"i64_less".
Definition _i64_nonnegative : ident := $"i64_nonnegative".
Definition _i__1 : ident := $"i__1".
Definition _i__2 : ident := $"i__2".
Definition _index : ident := $"index".
Definition _index_out : ident := $"index_out".
Definition _is_active_state : ident := $"is_active_state".
Definition _is_runnable : ident := $"is_runnable".
Definition _lhs : ident := $"lhs".
Definition _live_for_min : ident := $"live_for_min".
Definition _main : ident := $"main".
Definition _min_vruntime : ident := $"min_vruntime".
Definition _next : ident := $"next".
Definition _next__1 : ident := $"next__1".
Definition _next_time : ident := $"next_time".
Definition _ok_runqueue : ident := $"ok_runqueue".
Definition _out : ident := $"out".
Definition _pacha_eevdf_add : ident := $"pacha_eevdf_add".
Definition _pacha_eevdf_block : ident := $"pacha_eevdf_block".
Definition _pacha_eevdf_charge : ident := $"pacha_eevdf_charge".
Definition _pacha_eevdf_copy_runqueue : ident := $"pacha_eevdf_copy_runqueue".
Definition _pacha_eevdf_empty_entity : ident := $"pacha_eevdf_empty_entity".
Definition _pacha_eevdf_empty_runqueue : ident := $"pacha_eevdf_empty_runqueue".
Definition _pacha_eevdf_entity : ident := $"pacha_eevdf_entity".
Definition _pacha_eevdf_exit : ident := $"pacha_eevdf_exit".
Definition _pacha_eevdf_mark_running : ident := $"pacha_eevdf_mark_running".
Definition _pacha_eevdf_pick : ident := $"pacha_eevdf_pick".
Definition _pacha_eevdf_pick_result : ident := $"pacha_eevdf_pick_result".
Definition _pacha_eevdf_requeue_running : ident := $"pacha_eevdf_requeue_running".
Definition _pacha_eevdf_reset : ident := $"pacha_eevdf_reset".
Definition _pacha_eevdf_runqueue : ident := $"pacha_eevdf_runqueue".
Definition _pacha_eevdf_wake : ident := $"pacha_eevdf_wake".
Definition _place_entity_at_floor : ident := $"place_entity_at_floor".
Definition _quotient : ident := $"quotient".
Definition _rc : ident := $"rc".
Definition _recount_runqueue : ident := $"recount_runqueue".
Definition _refresh_deadline : ident := $"refresh_deadline".
Definition _refresh_runqueue : ident := $"refresh_runqueue".
Definition _refreshed : ident := $"refreshed".
Definition _remainder : ident := $"remainder".
Definition _result : ident := $"result".
Definition _rhs : ident := $"rhs".
Definition _rq : ident := $"rq".
Definition _runnable_count : ident := $"runnable_count".
Definition _runnable_or_running : ident := $"runnable_or_running".
Definition _runtime_ns : ident := $"runtime_ns".
Definition _service_ns : ident := $"service_ns".
Definition _set_entity_state : ident := $"set_entity_state".
Definition _slice : ident := $"slice".
Definition _slice_ns : ident := $"slice_ns".
Definition _src : ident := $"src".
Definition _state : ident := $"state".
Definition _thread_id : ident := $"thread_id".
Definition _valid_positive : ident := $"valid_positive".
Definition _value : ident := $"value".
Definition _virtual_time : ident := $"virtual_time".
Definition _vruntime : ident := $"vruntime".
Definition _weight : ident := $"weight".
Definition _weighted_delta : ident := $"weighted_delta".
Definition _weighted_fractional_10 : ident := $"weighted_fractional_10".
Definition _weighted_slice : ident := $"weighted_slice".
Definition _whole : ident := $"whole".
Definition _z_max : ident := $"z_max".
Definition _t'1 : ident := 128%positive.
Definition _t'10 : ident := 137%positive.
Definition _t'11 : ident := 138%positive.
Definition _t'12 : ident := 139%positive.
Definition _t'13 : ident := 140%positive.
Definition _t'14 : ident := 141%positive.
Definition _t'15 : ident := 142%positive.
Definition _t'16 : ident := 143%positive.
Definition _t'17 : ident := 144%positive.
Definition _t'18 : ident := 145%positive.
Definition _t'19 : ident := 146%positive.
Definition _t'2 : ident := 129%positive.
Definition _t'20 : ident := 147%positive.
Definition _t'21 : ident := 148%positive.
Definition _t'22 : ident := 149%positive.
Definition _t'23 : ident := 150%positive.
Definition _t'24 : ident := 151%positive.
Definition _t'25 : ident := 152%positive.
Definition _t'26 : ident := 153%positive.
Definition _t'27 : ident := 154%positive.
Definition _t'28 : ident := 155%positive.
Definition _t'3 : ident := 130%positive.
Definition _t'4 : ident := 131%positive.
Definition _t'5 : ident := 132%positive.
Definition _t'6 : ident := 133%positive.
Definition _t'7 : ident := 134%positive.
Definition _t'8 : ident := 135%positive.
Definition _t'9 : ident := 136%positive.

Definition f_valid_positive := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_value, tlong) :: nil);
  fn_vars := nil;
  fn_temps := nil;
  fn_body :=
(Sreturn (Some (Ebinop Ogt (Etempvar _value tlong)
                 (Econst_int (Int.repr 0) tint) tint)))
|}.

Definition f_i64_nonnegative := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_value, tlong) :: nil);
  fn_vars := nil;
  fn_temps := nil;
  fn_body :=
(Sreturn (Some (Ebinop Oge (Etempvar _value tlong)
                 (Econst_int (Int.repr 0) tint) tint)))
|}.

Definition f_i64_less := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_lhs, tlong) :: (_rhs, tlong) :: nil);
  fn_vars := nil;
  fn_temps := nil;
  fn_body :=
(Sreturn (Some (Ebinop Olt (Etempvar _lhs tlong) (Etempvar _rhs tlong) tint)))
|}.

Definition f_i64_equal := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_lhs, tlong) :: (_rhs, tlong) :: nil);
  fn_vars := nil;
  fn_temps := nil;
  fn_body :=
(Sreturn (Some (Ebinop Oeq (Etempvar _lhs tlong) (Etempvar _rhs tlong) tint)))
|}.

Definition f_checked_add_i64_overflows := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_lhs, tlong) :: (_rhs, tlong) :: nil);
  fn_vars := nil;
  fn_temps := ((_t'4, tint) :: (_t'3, tint) :: (_t'2, tint) ::
               (_t'1, tint) :: nil);
  fn_body :=
(Ssequence
  (Ssequence
    (Scall (Some _t'2)
      (Evar _i64_less (Tfunction (tlong :: tlong :: nil) tint cc_default))
      ((Econst_int (Int.repr 0) tint) :: (Etempvar _rhs tlong) :: nil))
    (Sifthenelse (Etempvar _t'2 tint)
      (Ssequence
        (Scall (Some _t'1)
          (Evar _i64_less (Tfunction (tlong :: tlong :: nil) tint cc_default))
          ((Ebinop Osub (Econst_long (Int64.repr 9223372036854775807) tlong)
             (Etempvar _rhs tlong) tlong) :: (Etempvar _lhs tlong) :: nil))
        (Sreturn (Some (Etempvar _t'1 tint))))
      Sskip))
  (Ssequence
    (Ssequence
      (Scall (Some _t'4)
        (Evar _i64_less (Tfunction (tlong :: tlong :: nil) tint cc_default))
        ((Etempvar _rhs tlong) :: (Econst_int (Int.repr 0) tint) :: nil))
      (Sifthenelse (Etempvar _t'4 tint)
        (Ssequence
          (Scall (Some _t'3)
            (Evar _i64_less (Tfunction (tlong :: tlong :: nil) tint
                              cc_default))
            ((Etempvar _lhs tlong) ::
             (Ebinop Osub
               (Ebinop Osub
                 (Eunop Oneg
                   (Econst_long (Int64.repr 9223372036854775807) tlong)
                   tlong) (Econst_int (Int.repr 1) tint) tlong)
               (Etempvar _rhs tlong) tlong) :: nil))
          (Sreturn (Some (Etempvar _t'3 tint))))
        Sskip))
    (Sreturn (Some (Econst_int (Int.repr 0) tint)))))
|}.

Definition f_checked_add_i64 := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_lhs, tlong) :: (_rhs, tlong) :: (_out, (tptr tlong)) ::
                nil);
  fn_vars := nil;
  fn_temps := ((_t'1, tint) :: nil);
  fn_body :=
(Ssequence
  (Ssequence
    (Scall (Some _t'1)
      (Evar _checked_add_i64_overflows (Tfunction (tlong :: tlong :: nil)
                                         tint cc_default))
      ((Etempvar _lhs tlong) :: (Etempvar _rhs tlong) :: nil))
    (Sifthenelse (Etempvar _t'1 tint)
      (Sreturn (Some (Econst_int (Int.repr 0) tint)))
      Sskip))
  (Ssequence
    (Sassign (Ederef (Etempvar _out (tptr tlong)) tlong)
      (Ebinop Oadd (Etempvar _lhs tlong) (Etempvar _rhs tlong) tlong))
    (Sreturn (Some (Econst_int (Int.repr 1) tint)))))
|}.

Definition f_checked_mul_i64_nonnegative_overflows := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_lhs, tlong) :: (_rhs, tlong) :: nil);
  fn_vars := nil;
  fn_temps := nil;
  fn_body :=
(Ssequence
  (Sifthenelse (Ebinop Oeq (Etempvar _rhs tlong)
                 (Econst_int (Int.repr 0) tint) tint)
    (Sreturn (Some (Econst_int (Int.repr 0) tint)))
    Sskip)
  (Sreturn (Some (Ebinop Ogt (Etempvar _lhs tlong)
                   (Ebinop Odiv
                     (Econst_long (Int64.repr 9223372036854775807) tlong)
                     (Etempvar _rhs tlong) tlong) tint))))
|}.

Definition f_checked_mul_i64_nonnegative := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_lhs, tlong) :: (_rhs, tlong) :: (_out, (tptr tlong)) ::
                nil);
  fn_vars := nil;
  fn_temps := ((_t'3, tint) :: (_t'2, tint) :: (_t'1, tint) :: nil);
  fn_body :=
(Ssequence
  (Ssequence
    (Scall (Some _t'1)
      (Evar _i64_nonnegative (Tfunction (tlong :: nil) tint cc_default))
      ((Etempvar _lhs tlong) :: nil))
    (Sifthenelse (Eunop Onotbool (Etempvar _t'1 tint) tint)
      (Sreturn (Some (Econst_int (Int.repr 0) tint)))
      Sskip))
  (Ssequence
    (Ssequence
      (Scall (Some _t'2)
        (Evar _i64_nonnegative (Tfunction (tlong :: nil) tint cc_default))
        ((Etempvar _rhs tlong) :: nil))
      (Sifthenelse (Eunop Onotbool (Etempvar _t'2 tint) tint)
        (Sreturn (Some (Econst_int (Int.repr 0) tint)))
        Sskip))
    (Ssequence
      (Ssequence
        (Scall (Some _t'3)
          (Evar _checked_mul_i64_nonnegative_overflows (Tfunction
                                                         (tlong :: tlong ::
                                                          nil) tint
                                                         cc_default))
          ((Etempvar _lhs tlong) :: (Etempvar _rhs tlong) :: nil))
        (Sifthenelse (Etempvar _t'3 tint)
          (Sreturn (Some (Econst_int (Int.repr 0) tint)))
          Sskip))
      (Ssequence
        (Sassign (Ederef (Etempvar _out (tptr tlong)) tlong)
          (Ebinop Omul (Etempvar _lhs tlong) (Etempvar _rhs tlong) tlong))
        (Sreturn (Some (Econst_int (Int.repr 1) tint)))))))
|}.

Definition f_z_max := {|
  fn_return := tlong;
  fn_callconv := cc_default;
  fn_params := ((_lhs, tlong) :: (_rhs, tlong) :: nil);
  fn_vars := nil;
  fn_temps := ((_t'1, tint) :: nil);
  fn_body :=
(Ssequence
  (Ssequence
    (Scall (Some _t'1)
      (Evar _i64_less (Tfunction (tlong :: tlong :: nil) tint cc_default))
      ((Etempvar _lhs tlong) :: (Etempvar _rhs tlong) :: nil))
    (Sifthenelse (Etempvar _t'1 tint)
      (Sreturn (Some (Etempvar _rhs tlong)))
      Sskip))
  (Sreturn (Some (Etempvar _lhs tlong))))
|}.

Definition f_is_runnable := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_entity, (tptr (Tstruct _pacha_eevdf_entity noattr))) ::
                nil);
  fn_vars := nil;
  fn_temps := ((_t'1, tint) :: nil);
  fn_body :=
(Ssequence
  (Sset _t'1
    (Efield
      (Ederef (Etempvar _entity (tptr (Tstruct _pacha_eevdf_entity noattr)))
        (Tstruct _pacha_eevdf_entity noattr)) _state tint))
  (Sreturn (Some (Ebinop Oeq (Etempvar _t'1 tint)
                   (Econst_int (Int.repr 1) tint) tint))))
|}.

Definition f_live_for_min := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_entity, (tptr (Tstruct _pacha_eevdf_entity noattr))) ::
                nil);
  fn_vars := nil;
  fn_temps := ((_t'1, tint) :: (_t'3, tint) :: (_t'2, tint) :: nil);
  fn_body :=
(Ssequence
  (Ssequence
    (Sset _t'2
      (Efield
        (Ederef
          (Etempvar _entity (tptr (Tstruct _pacha_eevdf_entity noattr)))
          (Tstruct _pacha_eevdf_entity noattr)) _state tint))
    (Sifthenelse (Ebinop Oeq (Etempvar _t'2 tint)
                   (Econst_int (Int.repr 1) tint) tint)
      (Sset _t'1 (Econst_int (Int.repr 1) tint))
      (Ssequence
        (Sset _t'3
          (Efield
            (Ederef
              (Etempvar _entity (tptr (Tstruct _pacha_eevdf_entity noattr)))
              (Tstruct _pacha_eevdf_entity noattr)) _state tint))
        (Sset _t'1
          (Ecast
            (Ebinop Oeq (Etempvar _t'3 tint) (Econst_int (Int.repr 2) tint)
              tint) tbool)))))
  (Sreturn (Some (Etempvar _t'1 tint))))
|}.

Definition f_is_active_state := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_state, tint) :: nil);
  fn_vars := nil;
  fn_temps := nil;
  fn_body :=
(Sreturn (Some (Ebinop One (Etempvar _state tint)
                 (Econst_int (Int.repr 0) tint) tint)))
|}.

Definition f_runnable_or_running := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_entity, (tptr (Tstruct _pacha_eevdf_entity noattr))) ::
                nil);
  fn_vars := nil;
  fn_temps := ((_t'1, tint) :: (_t'3, tint) :: (_t'2, tint) :: nil);
  fn_body :=
(Ssequence
  (Ssequence
    (Sset _t'2
      (Efield
        (Ederef
          (Etempvar _entity (tptr (Tstruct _pacha_eevdf_entity noattr)))
          (Tstruct _pacha_eevdf_entity noattr)) _state tint))
    (Sifthenelse (Ebinop Oeq (Etempvar _t'2 tint)
                   (Econst_int (Int.repr 1) tint) tint)
      (Sset _t'1 (Econst_int (Int.repr 1) tint))
      (Ssequence
        (Sset _t'3
          (Efield
            (Ederef
              (Etempvar _entity (tptr (Tstruct _pacha_eevdf_entity noattr)))
              (Tstruct _pacha_eevdf_entity noattr)) _state tint))
        (Sset _t'1
          (Ecast
            (Ebinop Oeq (Etempvar _t'3 tint) (Econst_int (Int.repr 2) tint)
              tint) tbool)))))
  (Sreturn (Some (Etempvar _t'1 tint))))
|}.

Definition f_ok_runqueue := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_rq, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                (_out, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) :: nil);
  fn_vars := nil;
  fn_temps := nil;
  fn_body :=
(Ssequence
  (Sassign
    (Ederef (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
      (Tstruct _pacha_eevdf_runqueue noattr))
    (Ederef (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
      (Tstruct _pacha_eevdf_runqueue noattr)))
  (Sreturn (Some (Econst_int (Int.repr 0) tint))))
|}.

Definition f_fail_runqueue := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_rc, tint) ::
                (_rq, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                (_out, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) :: nil);
  fn_vars := nil;
  fn_temps := nil;
  fn_body :=
(Ssequence
  (Sassign
    (Ederef (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
      (Tstruct _pacha_eevdf_runqueue noattr))
    (Ederef (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
      (Tstruct _pacha_eevdf_runqueue noattr)))
  (Sreturn (Some (Etempvar _rc tint))))
|}.

Definition f_pacha_eevdf_copy_runqueue := {|
  fn_return := tvoid;
  fn_callconv := cc_default;
  fn_params := ((_src, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                (_dst, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) :: nil);
  fn_vars := nil;
  fn_temps := ((_i, tulong) :: (_t'13, tlong) :: (_t'12, tlong) ::
               (_t'11, tlong) :: (_t'10, tlong) :: (_t'9, tlong) ::
               (_t'8, tlong) :: (_t'7, tlong) :: (_t'6, tlong) ::
               (_t'5, tint) :: (_t'4, tulong) :: (_t'3, tulong) ::
               (_t'2, tlong) :: (_t'1, tlong) :: nil);
  fn_body :=
(Ssequence
  (Ssequence
    (Sset _i (Ecast (Econst_int (Int.repr 0) tint) tulong))
    (Sloop
      (Ssequence
        (Sifthenelse (Ebinop Olt (Etempvar _i tulong)
                       (Econst_int (Int.repr 256) tuint) tint)
          Sskip
          Sbreak)
        (Ssequence
          (Ssequence
            (Sset _t'13
              (Efield
                (Ederef
                  (Ebinop Oadd
                    (Efield
                      (Ederef
                        (Etempvar _src (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                        (Tstruct _pacha_eevdf_runqueue noattr)) _entities
                      (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                    (Etempvar _i tulong)
                    (tptr (Tstruct _pacha_eevdf_entity noattr)))
                  (Tstruct _pacha_eevdf_entity noattr)) _thread_id tlong))
            (Sassign
              (Efield
                (Ederef
                  (Ebinop Oadd
                    (Efield
                      (Ederef
                        (Etempvar _dst (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                        (Tstruct _pacha_eevdf_runqueue noattr)) _entities
                      (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                    (Etempvar _i tulong)
                    (tptr (Tstruct _pacha_eevdf_entity noattr)))
                  (Tstruct _pacha_eevdf_entity noattr)) _thread_id tlong)
              (Etempvar _t'13 tlong)))
          (Ssequence
            (Ssequence
              (Sset _t'12
                (Efield
                  (Ederef
                    (Ebinop Oadd
                      (Efield
                        (Ederef
                          (Etempvar _src (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                          (Tstruct _pacha_eevdf_runqueue noattr)) _entities
                        (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                      (Etempvar _i tulong)
                      (tptr (Tstruct _pacha_eevdf_entity noattr)))
                    (Tstruct _pacha_eevdf_entity noattr)) _generation tlong))
              (Sassign
                (Efield
                  (Ederef
                    (Ebinop Oadd
                      (Efield
                        (Ederef
                          (Etempvar _dst (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                          (Tstruct _pacha_eevdf_runqueue noattr)) _entities
                        (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                      (Etempvar _i tulong)
                      (tptr (Tstruct _pacha_eevdf_entity noattr)))
                    (Tstruct _pacha_eevdf_entity noattr)) _generation tlong)
                (Etempvar _t'12 tlong)))
            (Ssequence
              (Ssequence
                (Sset _t'11
                  (Efield
                    (Ederef
                      (Ebinop Oadd
                        (Efield
                          (Ederef
                            (Etempvar _src (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                            (Tstruct _pacha_eevdf_runqueue noattr)) _entities
                          (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                        (Etempvar _i tulong)
                        (tptr (Tstruct _pacha_eevdf_entity noattr)))
                      (Tstruct _pacha_eevdf_entity noattr)) _weight tlong))
                (Sassign
                  (Efield
                    (Ederef
                      (Ebinop Oadd
                        (Efield
                          (Ederef
                            (Etempvar _dst (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                            (Tstruct _pacha_eevdf_runqueue noattr)) _entities
                          (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                        (Etempvar _i tulong)
                        (tptr (Tstruct _pacha_eevdf_entity noattr)))
                      (Tstruct _pacha_eevdf_entity noattr)) _weight tlong)
                  (Etempvar _t'11 tlong)))
              (Ssequence
                (Ssequence
                  (Sset _t'10
                    (Efield
                      (Ederef
                        (Ebinop Oadd
                          (Efield
                            (Ederef
                              (Etempvar _src (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                              (Tstruct _pacha_eevdf_runqueue noattr))
                            _entities
                            (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                          (Etempvar _i tulong)
                          (tptr (Tstruct _pacha_eevdf_entity noattr)))
                        (Tstruct _pacha_eevdf_entity noattr)) _slice_ns
                      tlong))
                  (Sassign
                    (Efield
                      (Ederef
                        (Ebinop Oadd
                          (Efield
                            (Ederef
                              (Etempvar _dst (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                              (Tstruct _pacha_eevdf_runqueue noattr))
                            _entities
                            (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                          (Etempvar _i tulong)
                          (tptr (Tstruct _pacha_eevdf_entity noattr)))
                        (Tstruct _pacha_eevdf_entity noattr)) _slice_ns
                      tlong) (Etempvar _t'10 tlong)))
                (Ssequence
                  (Ssequence
                    (Sset _t'9
                      (Efield
                        (Ederef
                          (Ebinop Oadd
                            (Efield
                              (Ederef
                                (Etempvar _src (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                                (Tstruct _pacha_eevdf_runqueue noattr))
                              _entities
                              (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                            (Etempvar _i tulong)
                            (tptr (Tstruct _pacha_eevdf_entity noattr)))
                          (Tstruct _pacha_eevdf_entity noattr)) _service_ns
                        tlong))
                    (Sassign
                      (Efield
                        (Ederef
                          (Ebinop Oadd
                            (Efield
                              (Ederef
                                (Etempvar _dst (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                                (Tstruct _pacha_eevdf_runqueue noattr))
                              _entities
                              (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                            (Etempvar _i tulong)
                            (tptr (Tstruct _pacha_eevdf_entity noattr)))
                          (Tstruct _pacha_eevdf_entity noattr)) _service_ns
                        tlong) (Etempvar _t'9 tlong)))
                  (Ssequence
                    (Ssequence
                      (Sset _t'8
                        (Efield
                          (Ederef
                            (Ebinop Oadd
                              (Efield
                                (Ederef
                                  (Etempvar _src (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                                  (Tstruct _pacha_eevdf_runqueue noattr))
                                _entities
                                (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                              (Etempvar _i tulong)
                              (tptr (Tstruct _pacha_eevdf_entity noattr)))
                            (Tstruct _pacha_eevdf_entity noattr)) _vruntime
                          tlong))
                      (Sassign
                        (Efield
                          (Ederef
                            (Ebinop Oadd
                              (Efield
                                (Ederef
                                  (Etempvar _dst (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                                  (Tstruct _pacha_eevdf_runqueue noattr))
                                _entities
                                (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                              (Etempvar _i tulong)
                              (tptr (Tstruct _pacha_eevdf_entity noattr)))
                            (Tstruct _pacha_eevdf_entity noattr)) _vruntime
                          tlong) (Etempvar _t'8 tlong)))
                    (Ssequence
                      (Ssequence
                        (Sset _t'7
                          (Efield
                            (Ederef
                              (Ebinop Oadd
                                (Efield
                                  (Ederef
                                    (Etempvar _src (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                                    (Tstruct _pacha_eevdf_runqueue noattr))
                                  _entities
                                  (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                                (Etempvar _i tulong)
                                (tptr (Tstruct _pacha_eevdf_entity noattr)))
                              (Tstruct _pacha_eevdf_entity noattr))
                            _eligible_time tlong))
                        (Sassign
                          (Efield
                            (Ederef
                              (Ebinop Oadd
                                (Efield
                                  (Ederef
                                    (Etempvar _dst (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                                    (Tstruct _pacha_eevdf_runqueue noattr))
                                  _entities
                                  (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                                (Etempvar _i tulong)
                                (tptr (Tstruct _pacha_eevdf_entity noattr)))
                              (Tstruct _pacha_eevdf_entity noattr))
                            _eligible_time tlong) (Etempvar _t'7 tlong)))
                      (Ssequence
                        (Ssequence
                          (Sset _t'6
                            (Efield
                              (Ederef
                                (Ebinop Oadd
                                  (Efield
                                    (Ederef
                                      (Etempvar _src (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                                      (Tstruct _pacha_eevdf_runqueue noattr))
                                    _entities
                                    (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                                  (Etempvar _i tulong)
                                  (tptr (Tstruct _pacha_eevdf_entity noattr)))
                                (Tstruct _pacha_eevdf_entity noattr))
                              _deadline tlong))
                          (Sassign
                            (Efield
                              (Ederef
                                (Ebinop Oadd
                                  (Efield
                                    (Ederef
                                      (Etempvar _dst (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                                      (Tstruct _pacha_eevdf_runqueue noattr))
                                    _entities
                                    (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                                  (Etempvar _i tulong)
                                  (tptr (Tstruct _pacha_eevdf_entity noattr)))
                                (Tstruct _pacha_eevdf_entity noattr))
                              _deadline tlong) (Etempvar _t'6 tlong)))
                        (Ssequence
                          (Sset _t'5
                            (Efield
                              (Ederef
                                (Ebinop Oadd
                                  (Efield
                                    (Ederef
                                      (Etempvar _src (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                                      (Tstruct _pacha_eevdf_runqueue noattr))
                                    _entities
                                    (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                                  (Etempvar _i tulong)
                                  (tptr (Tstruct _pacha_eevdf_entity noattr)))
                                (Tstruct _pacha_eevdf_entity noattr)) _state
                              tint))
                          (Sassign
                            (Efield
                              (Ederef
                                (Ebinop Oadd
                                  (Efield
                                    (Ederef
                                      (Etempvar _dst (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                                      (Tstruct _pacha_eevdf_runqueue noattr))
                                    _entities
                                    (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                                  (Etempvar _i tulong)
                                  (tptr (Tstruct _pacha_eevdf_entity noattr)))
                                (Tstruct _pacha_eevdf_entity noattr)) _state
                              tint) (Etempvar _t'5 tint))))))))))))
      (Sset _i
        (Ebinop Oadd (Etempvar _i tulong) (Econst_int (Int.repr 1) tint)
          tulong))))
  (Ssequence
    (Ssequence
      (Sset _t'4
        (Efield
          (Ederef
            (Etempvar _src (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
            (Tstruct _pacha_eevdf_runqueue noattr)) _entity_count tulong))
      (Sassign
        (Efield
          (Ederef
            (Etempvar _dst (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
            (Tstruct _pacha_eevdf_runqueue noattr)) _entity_count tulong)
        (Etempvar _t'4 tulong)))
    (Ssequence
      (Ssequence
        (Sset _t'3
          (Efield
            (Ederef
              (Etempvar _src (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
              (Tstruct _pacha_eevdf_runqueue noattr)) _runnable_count tulong))
        (Sassign
          (Efield
            (Ederef
              (Etempvar _dst (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
              (Tstruct _pacha_eevdf_runqueue noattr)) _runnable_count tulong)
          (Etempvar _t'3 tulong)))
      (Ssequence
        (Ssequence
          (Sset _t'2
            (Efield
              (Ederef
                (Etempvar _src (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                (Tstruct _pacha_eevdf_runqueue noattr)) _virtual_time tlong))
          (Sassign
            (Efield
              (Ederef
                (Etempvar _dst (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                (Tstruct _pacha_eevdf_runqueue noattr)) _virtual_time tlong)
            (Etempvar _t'2 tlong)))
        (Ssequence
          (Ssequence
            (Sset _t'1
              (Efield
                (Ederef
                  (Etempvar _src (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                  (Tstruct _pacha_eevdf_runqueue noattr)) _min_vruntime
                tlong))
            (Sassign
              (Efield
                (Ederef
                  (Etempvar _dst (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                  (Tstruct _pacha_eevdf_runqueue noattr)) _min_vruntime
                tlong) (Etempvar _t'1 tlong)))
          (Sreturn None))))))
|}.

Definition f_count_runnable := {|
  fn_return := tulong;
  fn_callconv := cc_default;
  fn_params := ((_rq, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) :: nil);
  fn_vars := nil;
  fn_temps := ((_count, tulong) :: (_i, tulong) :: (_t'1, tint) ::
               (_t'2, tulong) :: nil);
  fn_body :=
(Ssequence
  (Sset _count (Ecast (Econst_int (Int.repr 0) tint) tulong))
  (Ssequence
    (Ssequence
      (Sset _i (Ecast (Econst_int (Int.repr 0) tint) tulong))
      (Sloop
        (Ssequence
          (Ssequence
            (Sset _t'2
              (Efield
                (Ederef
                  (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                  (Tstruct _pacha_eevdf_runqueue noattr)) _entity_count
                tulong))
            (Sifthenelse (Ebinop Olt (Etempvar _i tulong)
                           (Etempvar _t'2 tulong) tint)
              Sskip
              Sbreak))
          (Ssequence
            (Scall (Some _t'1)
              (Evar _is_runnable (Tfunction
                                   ((tptr (Tstruct _pacha_eevdf_entity noattr)) ::
                                    nil) tint cc_default))
              ((Ebinop Oadd
                 (Efield
                   (Ederef
                     (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                     (Tstruct _pacha_eevdf_runqueue noattr)) _entities
                   (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                 (Etempvar _i tulong)
                 (tptr (Tstruct _pacha_eevdf_entity noattr))) :: nil))
            (Sifthenelse (Etempvar _t'1 tint)
              (Sset _count
                (Ebinop Oadd (Etempvar _count tulong)
                  (Econst_int (Int.repr 1) tint) tulong))
              Sskip)))
        (Sset _i
          (Ebinop Oadd (Etempvar _i tulong) (Econst_int (Int.repr 1) tint)
            tulong))))
    (Sreturn (Some (Etempvar _count tulong)))))
|}.

Definition f_weighted_fractional_10 := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_remainder, tlong) :: (_weight, tlong) ::
                (_out, (tptr tlong)) :: nil);
  fn_vars := nil;
  fn_temps := ((_fractional, tlong) :: (_bit, tint) :: nil);
  fn_body :=
(Ssequence
  (Sset _fractional (Ecast (Econst_int (Int.repr 0) tint) tlong))
  (Ssequence
    (Ssequence
      (Sset _bit (Econst_int (Int.repr 0) tint))
      (Sloop
        (Ssequence
          (Sifthenelse (Ebinop Olt (Etempvar _bit tint)
                         (Econst_int (Int.repr 10) tint) tint)
            Sskip
            Sbreak)
          (Ssequence
            (Sset _fractional
              (Ebinop Omul (Etempvar _fractional tlong)
                (Econst_int (Int.repr 2) tint) tlong))
            (Sifthenelse (Ebinop Oge (Etempvar _remainder tlong)
                           (Ebinop Osub (Etempvar _weight tlong)
                             (Etempvar _remainder tlong) tlong) tint)
              (Ssequence
                (Sset _fractional
                  (Ebinop Oadd (Etempvar _fractional tlong)
                    (Econst_int (Int.repr 1) tint) tlong))
                (Sset _remainder
                  (Ebinop Osub (Etempvar _remainder tlong)
                    (Ebinop Osub (Etempvar _weight tlong)
                      (Etempvar _remainder tlong) tlong) tlong)))
              (Sset _remainder
                (Ebinop Oadd (Etempvar _remainder tlong)
                  (Etempvar _remainder tlong) tlong)))))
        (Sset _bit
          (Ebinop Oadd (Etempvar _bit tint) (Econst_int (Int.repr 1) tint)
            tint))))
    (Ssequence
      (Sassign (Ederef (Etempvar _out (tptr tlong)) tlong)
        (Etempvar _fractional tlong))
      (Sreturn (Some (Econst_int (Int.repr 1) tint))))))
|}.

Definition f_weighted_delta := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_runtime_ns, tlong) :: (_weight, tlong) ::
                (_out, (tptr tlong)) :: nil);
  fn_vars := ((_whole, tlong) :: (_fractional, tlong) :: (_delta, tlong) ::
              nil);
  fn_temps := ((_quotient, tlong) :: (_remainder, tlong) :: (_t'5, tint) ::
               (_t'4, tint) :: (_t'3, tint) :: (_t'2, tint) ::
               (_t'1, tint) :: (_t'8, tlong) :: (_t'7, tlong) ::
               (_t'6, tlong) :: nil);
  fn_body :=
(Ssequence
  (Ssequence
    (Scall (Some _t'1)
      (Evar _i64_nonnegative (Tfunction (tlong :: nil) tint cc_default))
      ((Etempvar _runtime_ns tlong) :: nil))
    (Sifthenelse (Eunop Onotbool (Etempvar _t'1 tint) tint)
      (Sreturn (Some (Econst_int (Int.repr 0) tint)))
      Sskip))
  (Ssequence
    (Ssequence
      (Scall (Some _t'2)
        (Evar _valid_positive (Tfunction (tlong :: nil) tint cc_default))
        ((Etempvar _weight tlong) :: nil))
      (Sifthenelse (Eunop Onotbool (Etempvar _t'2 tint) tint)
        (Sreturn (Some (Econst_int (Int.repr 0) tint)))
        Sskip))
    (Ssequence
      (Sset _quotient
        (Ebinop Odiv (Etempvar _runtime_ns tlong) (Etempvar _weight tlong)
          tlong))
      (Ssequence
        (Sset _remainder
          (Ebinop Omod (Etempvar _runtime_ns tlong) (Etempvar _weight tlong)
            tlong))
        (Ssequence
          (Sassign (Evar _whole tlong) (Econst_int (Int.repr 0) tint))
          (Ssequence
            (Ssequence
              (Scall (Some _t'3)
                (Evar _checked_mul_i64_nonnegative (Tfunction
                                                     (tlong :: tlong ::
                                                      (tptr tlong) :: nil)
                                                     tint cc_default))
                ((Etempvar _quotient tlong) ::
                 (Econst_int (Int.repr 1024) tint) ::
                 (Eaddrof (Evar _whole tlong) (tptr tlong)) :: nil))
              (Sifthenelse (Eunop Onotbool (Etempvar _t'3 tint) tint)
                (Sreturn (Some (Econst_int (Int.repr 0) tint)))
                Sskip))
            (Ssequence
              (Sassign (Evar _fractional tlong)
                (Econst_int (Int.repr 0) tint))
              (Ssequence
                (Ssequence
                  (Scall (Some _t'4)
                    (Evar _weighted_fractional_10 (Tfunction
                                                    (tlong :: tlong ::
                                                     (tptr tlong) :: nil)
                                                    tint cc_default))
                    ((Etempvar _remainder tlong) ::
                     (Etempvar _weight tlong) ::
                     (Eaddrof (Evar _fractional tlong) (tptr tlong)) :: nil))
                  (Sifthenelse (Eunop Onotbool (Etempvar _t'4 tint) tint)
                    (Sreturn (Some (Econst_int (Int.repr 0) tint)))
                    Sskip))
                (Ssequence
                  (Sassign (Evar _delta tlong)
                    (Econst_int (Int.repr 0) tint))
                  (Ssequence
                    (Ssequence
                      (Ssequence
                        (Sset _t'7 (Evar _whole tlong))
                        (Ssequence
                          (Sset _t'8 (Evar _fractional tlong))
                          (Scall (Some _t'5)
                            (Evar _checked_add_i64 (Tfunction
                                                     (tlong :: tlong ::
                                                      (tptr tlong) :: nil)
                                                     tint cc_default))
                            ((Etempvar _t'7 tlong) ::
                             (Etempvar _t'8 tlong) ::
                             (Eaddrof (Evar _delta tlong) (tptr tlong)) ::
                             nil))))
                      (Sifthenelse (Eunop Onotbool (Etempvar _t'5 tint) tint)
                        (Sreturn (Some (Econst_int (Int.repr 0) tint)))
                        Sskip))
                    (Ssequence
                      (Ssequence
                        (Sset _t'6 (Evar _delta tlong))
                        (Sassign (Ederef (Etempvar _out (tptr tlong)) tlong)
                          (Etempvar _t'6 tlong)))
                      (Sreturn (Some (Econst_int (Int.repr 1) tint))))))))))))))
|}.

Definition f_weighted_slice := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_slice_ns, tlong) :: (_weight, tlong) ::
                (_out, (tptr tlong)) :: nil);
  fn_vars := ((_delta, tlong) :: nil);
  fn_temps := ((_t'2, tlong) :: (_t'1, tint) :: (_t'3, tlong) :: nil);
  fn_body :=
(Ssequence
  (Ssequence
    (Scall (Some _t'1)
      (Evar _weighted_delta (Tfunction
                              (tlong :: tlong :: (tptr tlong) :: nil) tint
                              cc_default))
      ((Etempvar _slice_ns tlong) :: (Etempvar _weight tlong) ::
       (Eaddrof (Evar _delta tlong) (tptr tlong)) :: nil))
    (Sifthenelse (Eunop Onotbool (Etempvar _t'1 tint) tint)
      (Sreturn (Some (Econst_int (Int.repr 0) tint)))
      Sskip))
  (Ssequence
    (Ssequence
      (Ssequence
        (Sset _t'3 (Evar _delta tlong))
        (Scall (Some _t'2)
          (Evar _z_max (Tfunction (tlong :: tlong :: nil) tlong cc_default))
          ((Econst_int (Int.repr 1) tint) :: (Etempvar _t'3 tlong) :: nil)))
      (Sassign (Ederef (Etempvar _out (tptr tlong)) tlong)
        (Etempvar _t'2 tlong)))
    (Sreturn (Some (Econst_int (Int.repr 1) tint)))))
|}.

Definition f_refresh_deadline := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_entity, (tptr (Tstruct _pacha_eevdf_entity noattr))) ::
                (_floor_vruntime, tlong) ::
                (_out, (tptr (Tstruct _pacha_eevdf_entity noattr))) :: nil);
  fn_vars := ((_slice, tlong) :: (_deadline, tlong) :: nil);
  fn_temps := ((_eligible, tlong) :: (_t'3, tint) :: (_t'2, tlong) ::
               (_t'1, tint) :: (_t'8, tlong) :: (_t'7, tlong) ::
               (_t'6, tlong) :: (_t'5, tlong) :: (_t'4, tlong) :: nil);
  fn_body :=
(Ssequence
  (Ssequence
    (Ssequence
      (Sset _t'7
        (Efield
          (Ederef
            (Etempvar _entity (tptr (Tstruct _pacha_eevdf_entity noattr)))
            (Tstruct _pacha_eevdf_entity noattr)) _slice_ns tlong))
      (Ssequence
        (Sset _t'8
          (Efield
            (Ederef
              (Etempvar _entity (tptr (Tstruct _pacha_eevdf_entity noattr)))
              (Tstruct _pacha_eevdf_entity noattr)) _weight tlong))
        (Scall (Some _t'1)
          (Evar _weighted_slice (Tfunction
                                  (tlong :: tlong :: (tptr tlong) :: nil)
                                  tint cc_default))
          ((Etempvar _t'7 tlong) :: (Etempvar _t'8 tlong) ::
           (Eaddrof (Evar _slice tlong) (tptr tlong)) :: nil))))
    (Sifthenelse (Eunop Onotbool (Etempvar _t'1 tint) tint)
      (Sreturn (Some (Econst_int (Int.repr 0) tint)))
      Sskip))
  (Ssequence
    (Ssequence
      (Ssequence
        (Sset _t'6
          (Efield
            (Ederef
              (Etempvar _entity (tptr (Tstruct _pacha_eevdf_entity noattr)))
              (Tstruct _pacha_eevdf_entity noattr)) _vruntime tlong))
        (Scall (Some _t'2)
          (Evar _z_max (Tfunction (tlong :: tlong :: nil) tlong cc_default))
          ((Etempvar _t'6 tlong) :: (Etempvar _floor_vruntime tlong) :: nil)))
      (Sset _eligible (Etempvar _t'2 tlong)))
    (Ssequence
      (Ssequence
        (Ssequence
          (Sset _t'5 (Evar _slice tlong))
          (Scall (Some _t'3)
            (Evar _checked_add_i64 (Tfunction
                                     (tlong :: tlong :: (tptr tlong) :: nil)
                                     tint cc_default))
            ((Etempvar _eligible tlong) :: (Etempvar _t'5 tlong) ::
             (Eaddrof (Evar _deadline tlong) (tptr tlong)) :: nil)))
        (Sifthenelse (Eunop Onotbool (Etempvar _t'3 tint) tint)
          (Sreturn (Some (Econst_int (Int.repr 0) tint)))
          Sskip))
      (Ssequence
        (Sassign
          (Ederef (Etempvar _out (tptr (Tstruct _pacha_eevdf_entity noattr)))
            (Tstruct _pacha_eevdf_entity noattr))
          (Ederef
            (Etempvar _entity (tptr (Tstruct _pacha_eevdf_entity noattr)))
            (Tstruct _pacha_eevdf_entity noattr)))
        (Ssequence
          (Sassign
            (Efield
              (Ederef
                (Etempvar _out (tptr (Tstruct _pacha_eevdf_entity noattr)))
                (Tstruct _pacha_eevdf_entity noattr)) _eligible_time tlong)
            (Etempvar _eligible tlong))
          (Ssequence
            (Ssequence
              (Sset _t'4 (Evar _deadline tlong))
              (Sassign
                (Efield
                  (Ederef
                    (Etempvar _out (tptr (Tstruct _pacha_eevdf_entity noattr)))
                    (Tstruct _pacha_eevdf_entity noattr)) _deadline tlong)
                (Etempvar _t'4 tlong)))
            (Sreturn (Some (Econst_int (Int.repr 1) tint)))))))))
|}.

Definition f_refresh_runqueue := {|
  fn_return := tvoid;
  fn_callconv := cc_default;
  fn_params := ((_rq, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) :: nil);
  fn_vars := nil;
  fn_temps := ((_found, tint) :: (_min_vruntime, tlong) :: (_i, tulong) ::
               (_t'4, tlong) :: (_t'3, tulong) :: (_t'2, tint) ::
               (_t'1, tint) :: (_t'7, tulong) :: (_t'6, tlong) ::
               (_t'5, tlong) :: nil);
  fn_body :=
(Ssequence
  (Sset _found (Econst_int (Int.repr 0) tint))
  (Ssequence
    (Sset _min_vruntime
      (Efield
        (Ederef (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
          (Tstruct _pacha_eevdf_runqueue noattr)) _min_vruntime tlong))
    (Ssequence
      (Ssequence
        (Sset _i (Ecast (Econst_int (Int.repr 0) tint) tulong))
        (Sloop
          (Ssequence
            (Ssequence
              (Sset _t'7
                (Efield
                  (Ederef
                    (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                    (Tstruct _pacha_eevdf_runqueue noattr)) _entity_count
                  tulong))
              (Sifthenelse (Ebinop Olt (Etempvar _i tulong)
                             (Etempvar _t'7 tulong) tint)
                Sskip
                Sbreak))
            (Ssequence
              (Ssequence
                (Scall (Some _t'1)
                  (Evar _live_for_min (Tfunction
                                        ((tptr (Tstruct _pacha_eevdf_entity noattr)) ::
                                         nil) tint cc_default))
                  ((Ebinop Oadd
                     (Efield
                       (Ederef
                         (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                         (Tstruct _pacha_eevdf_runqueue noattr)) _entities
                       (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                     (Etempvar _i tulong)
                     (tptr (Tstruct _pacha_eevdf_entity noattr))) :: nil))
                (Sifthenelse (Eunop Onotbool (Etempvar _t'1 tint) tint)
                  Scontinue
                  Sskip))
              (Ssequence
                (Ssequence
                  (Sifthenelse (Eunop Onotbool (Etempvar _found tint) tint)
                    (Sset _t'2 (Econst_int (Int.repr 1) tint))
                    (Ssequence
                      (Sset _t'6
                        (Efield
                          (Ederef
                            (Ebinop Oadd
                              (Efield
                                (Ederef
                                  (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                                  (Tstruct _pacha_eevdf_runqueue noattr))
                                _entities
                                (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                              (Etempvar _i tulong)
                              (tptr (Tstruct _pacha_eevdf_entity noattr)))
                            (Tstruct _pacha_eevdf_entity noattr)) _vruntime
                          tlong))
                      (Sset _t'2
                        (Ecast
                          (Ebinop Olt (Etempvar _t'6 tlong)
                            (Etempvar _min_vruntime tlong) tint) tbool))))
                  (Sifthenelse (Etempvar _t'2 tint)
                    (Sset _min_vruntime
                      (Efield
                        (Ederef
                          (Ebinop Oadd
                            (Efield
                              (Ederef
                                (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                                (Tstruct _pacha_eevdf_runqueue noattr))
                              _entities
                              (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                            (Etempvar _i tulong)
                            (tptr (Tstruct _pacha_eevdf_entity noattr)))
                          (Tstruct _pacha_eevdf_entity noattr)) _vruntime
                        tlong))
                    Sskip))
                (Sset _found (Econst_int (Int.repr 1) tint)))))
          (Sset _i
            (Ebinop Oadd (Etempvar _i tulong) (Econst_int (Int.repr 1) tint)
              tulong))))
      (Ssequence
        (Ssequence
          (Scall (Some _t'3)
            (Evar _count_runnable (Tfunction
                                    ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                     nil) tulong cc_default))
            ((Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
             nil))
          (Sassign
            (Efield
              (Ederef
                (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                (Tstruct _pacha_eevdf_runqueue noattr)) _runnable_count
              tulong) (Etempvar _t'3 tulong)))
        (Ssequence
          (Ssequence
            (Ssequence
              (Sset _t'5
                (Efield
                  (Ederef
                    (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                    (Tstruct _pacha_eevdf_runqueue noattr)) _virtual_time
                  tlong))
              (Scall (Some _t'4)
                (Evar _z_max (Tfunction (tlong :: tlong :: nil) tlong
                               cc_default))
                ((Etempvar _t'5 tlong) :: (Etempvar _min_vruntime tlong) ::
                 nil)))
            (Sassign
              (Efield
                (Ederef
                  (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                  (Tstruct _pacha_eevdf_runqueue noattr)) _virtual_time
                tlong) (Etempvar _t'4 tlong)))
          (Sassign
            (Efield
              (Ederef
                (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                (Tstruct _pacha_eevdf_runqueue noattr)) _min_vruntime tlong)
            (Etempvar _min_vruntime tlong)))))))
|}.

Definition f_recount_runqueue := {|
  fn_return := tvoid;
  fn_callconv := cc_default;
  fn_params := ((_rq, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) :: nil);
  fn_vars := nil;
  fn_temps := ((_t'1, tulong) :: nil);
  fn_body :=
(Ssequence
  (Scall (Some _t'1)
    (Evar _count_runnable (Tfunction
                            ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                             nil) tulong cc_default))
    ((Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr))) :: nil))
  (Sassign
    (Efield
      (Ederef (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
        (Tstruct _pacha_eevdf_runqueue noattr)) _runnable_count tulong)
    (Etempvar _t'1 tulong)))
|}.

Definition f_set_entity_state := {|
  fn_return := tvoid;
  fn_callconv := cc_default;
  fn_params := ((_entity, (tptr (Tstruct _pacha_eevdf_entity noattr))) ::
                (_state, tint) :: nil);
  fn_vars := nil;
  fn_temps := nil;
  fn_body :=
(Sassign
  (Efield
    (Ederef (Etempvar _entity (tptr (Tstruct _pacha_eevdf_entity noattr)))
      (Tstruct _pacha_eevdf_entity noattr)) _state tint)
  (Etempvar _state tint))
|}.

Definition f_place_entity_at_floor := {|
  fn_return := tvoid;
  fn_callconv := cc_default;
  fn_params := ((_entity, (tptr (Tstruct _pacha_eevdf_entity noattr))) ::
                (_floor_vruntime, tlong) :: nil);
  fn_vars := nil;
  fn_temps := ((_t'1, tlong) :: (_t'2, tlong) :: nil);
  fn_body :=
(Ssequence
  (Ssequence
    (Sset _t'2
      (Efield
        (Ederef
          (Etempvar _entity (tptr (Tstruct _pacha_eevdf_entity noattr)))
          (Tstruct _pacha_eevdf_entity noattr)) _vruntime tlong))
    (Scall (Some _t'1)
      (Evar _z_max (Tfunction (tlong :: tlong :: nil) tlong cc_default))
      ((Etempvar _t'2 tlong) :: (Etempvar _floor_vruntime tlong) :: nil)))
  (Sassign
    (Efield
      (Ederef (Etempvar _entity (tptr (Tstruct _pacha_eevdf_entity noattr)))
        (Tstruct _pacha_eevdf_entity noattr)) _vruntime tlong)
    (Etempvar _t'1 tlong)))
|}.

Definition f_find_entity_index := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_rq, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                (_thread_id, tlong) :: (_index_out, (tptr tulong)) :: nil);
  fn_vars := nil;
  fn_temps := ((_i, tulong) :: (_t'2, tint) :: (_t'1, tint) ::
               (_t'5, tulong) :: (_t'4, tint) :: (_t'3, tlong) :: nil);
  fn_body :=
(Ssequence
  (Sifthenelse (Ebinop Oeq (Etempvar _thread_id tlong)
                 (Econst_int (Int.repr 0) tint) tint)
    (Sreturn (Some (Econst_int (Int.repr 0) tint)))
    Sskip)
  (Ssequence
    (Ssequence
      (Sset _i (Ecast (Econst_int (Int.repr 0) tint) tulong))
      (Sloop
        (Ssequence
          (Ssequence
            (Sset _t'5
              (Efield
                (Ederef
                  (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                  (Tstruct _pacha_eevdf_runqueue noattr)) _entity_count
                tulong))
            (Sifthenelse (Ebinop Olt (Etempvar _i tulong)
                           (Etempvar _t'5 tulong) tint)
              Sskip
              Sbreak))
          (Ssequence
            (Ssequence
              (Sset _t'3
                (Efield
                  (Ederef
                    (Ebinop Oadd
                      (Efield
                        (Ederef
                          (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                          (Tstruct _pacha_eevdf_runqueue noattr)) _entities
                        (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                      (Etempvar _i tulong)
                      (tptr (Tstruct _pacha_eevdf_entity noattr)))
                    (Tstruct _pacha_eevdf_entity noattr)) _thread_id tlong))
              (Sifthenelse (Ebinop Oeq (Etempvar _t'3 tlong)
                             (Etempvar _thread_id tlong) tint)
                (Ssequence
                  (Ssequence
                    (Sset _t'4
                      (Efield
                        (Ederef
                          (Ebinop Oadd
                            (Efield
                              (Ederef
                                (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                                (Tstruct _pacha_eevdf_runqueue noattr))
                              _entities
                              (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                            (Etempvar _i tulong)
                            (tptr (Tstruct _pacha_eevdf_entity noattr)))
                          (Tstruct _pacha_eevdf_entity noattr)) _state tint))
                    (Scall (Some _t'2)
                      (Evar _is_active_state (Tfunction (tint :: nil) tint
                                               cc_default))
                      ((Etempvar _t'4 tint) :: nil)))
                  (Sset _t'1 (Ecast (Etempvar _t'2 tint) tbool)))
                (Sset _t'1 (Econst_int (Int.repr 0) tint))))
            (Sifthenelse (Etempvar _t'1 tint)
              (Ssequence
                (Sassign (Ederef (Etempvar _index_out (tptr tulong)) tulong)
                  (Etempvar _i tulong))
                (Sreturn (Some (Econst_int (Int.repr 1) tint))))
              Sskip)))
        (Sset _i
          (Ebinop Oadd (Etempvar _i tulong) (Econst_int (Int.repr 1) tint)
            tulong))))
    (Sreturn (Some (Econst_int (Int.repr 0) tint)))))
|}.

Definition f_entity_better_values := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_candidate_deadline, tlong) :: (_current_deadline, tlong) ::
                (_candidate_thread_id, tlong) ::
                (_current_thread_id, tlong) :: nil);
  fn_vars := nil;
  fn_temps := ((_result, tint) :: (_t'3, tint) :: (_t'2, tint) ::
               (_t'1, tint) :: nil);
  fn_body :=
(Ssequence
  (Sset _result (Econst_int (Int.repr 0) tint))
  (Ssequence
    (Ssequence
      (Scall (Some _t'3)
        (Evar _i64_less (Tfunction (tlong :: tlong :: nil) tint cc_default))
        ((Etempvar _candidate_deadline tlong) ::
         (Etempvar _current_deadline tlong) :: nil))
      (Sifthenelse (Etempvar _t'3 tint)
        (Sset _result (Econst_int (Int.repr 1) tint))
        (Ssequence
          (Scall (Some _t'2)
            (Evar _i64_equal (Tfunction (tlong :: tlong :: nil) tint
                               cc_default))
            ((Etempvar _candidate_deadline tlong) ::
             (Etempvar _current_deadline tlong) :: nil))
          (Sifthenelse (Etempvar _t'2 tint)
            (Ssequence
              (Scall (Some _t'1)
                (Evar _i64_less (Tfunction (tlong :: tlong :: nil) tint
                                  cc_default))
                ((Etempvar _candidate_thread_id tlong) ::
                 (Etempvar _current_thread_id tlong) :: nil))
              (Sset _result (Etempvar _t'1 tint)))
            Sskip))))
    (Sreturn (Some (Etempvar _result tint)))))
|}.

Definition f_entity_better := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_candidate, (tptr (Tstruct _pacha_eevdf_entity noattr))) ::
                (_current, (tptr (Tstruct _pacha_eevdf_entity noattr))) ::
                nil);
  fn_vars := nil;
  fn_temps := ((_result, tint) :: (_t'1, tint) :: (_t'5, tlong) ::
               (_t'4, tlong) :: (_t'3, tlong) :: (_t'2, tlong) :: nil);
  fn_body :=
(Ssequence
  (Ssequence
    (Ssequence
      (Sset _t'2
        (Efield
          (Ederef
            (Etempvar _candidate (tptr (Tstruct _pacha_eevdf_entity noattr)))
            (Tstruct _pacha_eevdf_entity noattr)) _deadline tlong))
      (Ssequence
        (Sset _t'3
          (Efield
            (Ederef
              (Etempvar _current (tptr (Tstruct _pacha_eevdf_entity noattr)))
              (Tstruct _pacha_eevdf_entity noattr)) _deadline tlong))
        (Ssequence
          (Sset _t'4
            (Efield
              (Ederef
                (Etempvar _candidate (tptr (Tstruct _pacha_eevdf_entity noattr)))
                (Tstruct _pacha_eevdf_entity noattr)) _thread_id tlong))
          (Ssequence
            (Sset _t'5
              (Efield
                (Ederef
                  (Etempvar _current (tptr (Tstruct _pacha_eevdf_entity noattr)))
                  (Tstruct _pacha_eevdf_entity noattr)) _thread_id tlong))
            (Scall (Some _t'1)
              (Evar _entity_better_values (Tfunction
                                            (tlong :: tlong :: tlong ::
                                             tlong :: nil) tint cc_default))
              ((Etempvar _t'2 tlong) :: (Etempvar _t'3 tlong) ::
               (Etempvar _t'4 tlong) :: (Etempvar _t'5 tlong) :: nil))))))
    (Sset _result (Etempvar _t'1 tint)))
  (Sreturn (Some (Etempvar _result tint))))
|}.

Definition f_pacha_eevdf_empty_entity := {|
  fn_return := tvoid;
  fn_callconv := cc_default;
  fn_params := ((_out, (tptr (Tstruct _pacha_eevdf_entity noattr))) :: nil);
  fn_vars := nil;
  fn_temps := nil;
  fn_body :=
(Ssequence
  (Sassign
    (Efield
      (Ederef (Etempvar _out (tptr (Tstruct _pacha_eevdf_entity noattr)))
        (Tstruct _pacha_eevdf_entity noattr)) _thread_id tlong)
    (Econst_int (Int.repr 0) tint))
  (Ssequence
    (Sassign
      (Efield
        (Ederef (Etempvar _out (tptr (Tstruct _pacha_eevdf_entity noattr)))
          (Tstruct _pacha_eevdf_entity noattr)) _generation tlong)
      (Econst_int (Int.repr 0) tint))
    (Ssequence
      (Sassign
        (Efield
          (Ederef (Etempvar _out (tptr (Tstruct _pacha_eevdf_entity noattr)))
            (Tstruct _pacha_eevdf_entity noattr)) _weight tlong)
        (Econst_int (Int.repr 0) tint))
      (Ssequence
        (Sassign
          (Efield
            (Ederef
              (Etempvar _out (tptr (Tstruct _pacha_eevdf_entity noattr)))
              (Tstruct _pacha_eevdf_entity noattr)) _slice_ns tlong)
          (Econst_int (Int.repr 0) tint))
        (Ssequence
          (Sassign
            (Efield
              (Ederef
                (Etempvar _out (tptr (Tstruct _pacha_eevdf_entity noattr)))
                (Tstruct _pacha_eevdf_entity noattr)) _service_ns tlong)
            (Econst_int (Int.repr 0) tint))
          (Ssequence
            (Sassign
              (Efield
                (Ederef
                  (Etempvar _out (tptr (Tstruct _pacha_eevdf_entity noattr)))
                  (Tstruct _pacha_eevdf_entity noattr)) _vruntime tlong)
              (Econst_int (Int.repr 0) tint))
            (Ssequence
              (Sassign
                (Efield
                  (Ederef
                    (Etempvar _out (tptr (Tstruct _pacha_eevdf_entity noattr)))
                    (Tstruct _pacha_eevdf_entity noattr)) _eligible_time
                  tlong) (Econst_int (Int.repr 0) tint))
              (Ssequence
                (Sassign
                  (Efield
                    (Ederef
                      (Etempvar _out (tptr (Tstruct _pacha_eevdf_entity noattr)))
                      (Tstruct _pacha_eevdf_entity noattr)) _deadline tlong)
                  (Econst_int (Int.repr 0) tint))
                (Ssequence
                  (Sassign
                    (Efield
                      (Ederef
                        (Etempvar _out (tptr (Tstruct _pacha_eevdf_entity noattr)))
                        (Tstruct _pacha_eevdf_entity noattr)) _state tint)
                    (Econst_int (Int.repr 0) tint))
                  (Sreturn None))))))))))
|}.

Definition f_pacha_eevdf_empty_runqueue := {|
  fn_return := tvoid;
  fn_callconv := cc_default;
  fn_params := ((_out, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) :: nil);
  fn_vars := nil;
  fn_temps := ((_i, tulong) :: nil);
  fn_body :=
(Ssequence
  (Ssequence
    (Sset _i (Ecast (Econst_int (Int.repr 0) tint) tulong))
    (Sloop
      (Ssequence
        (Sifthenelse (Ebinop Olt (Etempvar _i tulong)
                       (Econst_int (Int.repr 256) tuint) tint)
          Sskip
          Sbreak)
        (Scall None
          (Evar _pacha_eevdf_empty_entity (Tfunction
                                            ((tptr (Tstruct _pacha_eevdf_entity noattr)) ::
                                             nil) tvoid cc_default))
          ((Ebinop Oadd
             (Efield
               (Ederef
                 (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                 (Tstruct _pacha_eevdf_runqueue noattr)) _entities
               (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
             (Etempvar _i tulong)
             (tptr (Tstruct _pacha_eevdf_entity noattr))) :: nil)))
      (Sset _i
        (Ebinop Oadd (Etempvar _i tulong) (Econst_int (Int.repr 1) tint)
          tulong))))
  (Ssequence
    (Sassign
      (Efield
        (Ederef (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
          (Tstruct _pacha_eevdf_runqueue noattr)) _entity_count tulong)
      (Econst_int (Int.repr 0) tint))
    (Ssequence
      (Sassign
        (Efield
          (Ederef
            (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
            (Tstruct _pacha_eevdf_runqueue noattr)) _runnable_count tulong)
        (Econst_int (Int.repr 0) tint))
      (Ssequence
        (Sassign
          (Efield
            (Ederef
              (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
              (Tstruct _pacha_eevdf_runqueue noattr)) _virtual_time tlong)
          (Econst_int (Int.repr 0) tint))
        (Ssequence
          (Sassign
            (Efield
              (Ederef
                (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                (Tstruct _pacha_eevdf_runqueue noattr)) _min_vruntime tlong)
            (Econst_int (Int.repr 0) tint))
          (Sreturn None))))))
|}.

Definition f_pacha_eevdf_reset := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_rq, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                (_out, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) :: nil);
  fn_vars := nil;
  fn_temps := nil;
  fn_body :=
(Ssequence
  (Scall None
    (Evar _pacha_eevdf_empty_runqueue (Tfunction
                                        ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                         nil) tvoid cc_default))
    ((Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr))) :: nil))
  (Sreturn (Some (Econst_int (Int.repr 0) tint))))
|}.

Definition f_pacha_eevdf_add := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_rq, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                (_thread_id, tlong) :: (_generation, tlong) ::
                (_weight, tlong) :: (_slice_ns, tlong) ::
                (_out, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) :: nil);
  fn_vars := ((_existing, tulong) ::
              (_entity, (Tstruct _pacha_eevdf_entity noattr)) ::
              (_refreshed, (Tstruct _pacha_eevdf_entity noattr)) ::
              (_next, (Tstruct _pacha_eevdf_runqueue noattr)) ::
              (_next__1, (Tstruct _pacha_eevdf_runqueue noattr)) :: nil);
  fn_temps := ((_t'14, tint) :: (_t'13, tint) :: (_t'12, tint) ::
               (_t'11, tint) :: (_t'10, tint) :: (_t'9, tint) ::
               (_t'8, tint) :: (_t'7, tint) :: (_t'6, tint) ::
               (_t'5, tint) :: (_t'4, tint) :: (_t'3, tint) ::
               (_t'2, tint) :: (_t'1, tint) :: (_t'28, tint) ::
               (_t'27, tulong) :: (_t'26, tlong) :: (_t'25, tlong) ::
               (_t'24, tlong) :: (_t'23, tlong) :: (_t'22, tulong) ::
               (_t'21, tulong) :: (_t'20, tlong) :: (_t'19, tlong) ::
               (_t'18, tlong) :: (_t'17, tlong) :: (_t'16, tulong) ::
               (_t'15, tulong) :: nil);
  fn_body :=
(Ssequence
  (Ssequence
    (Ssequence
      (Sifthenelse (Ebinop Oeq (Etempvar _thread_id tlong)
                     (Econst_int (Int.repr 0) tint) tint)
        (Sset _t'2 (Econst_int (Int.repr 1) tint))
        (Ssequence
          (Scall (Some _t'3)
            (Evar _valid_positive (Tfunction (tlong :: nil) tint cc_default))
            ((Etempvar _weight tlong) :: nil))
          (Sset _t'2
            (Ecast (Eunop Onotbool (Etempvar _t'3 tint) tint) tbool))))
      (Sifthenelse (Etempvar _t'2 tint)
        (Sset _t'4 (Econst_int (Int.repr 1) tint))
        (Ssequence
          (Scall (Some _t'5)
            (Evar _valid_positive (Tfunction (tlong :: nil) tint cc_default))
            ((Etempvar _slice_ns tlong) :: nil))
          (Sset _t'4
            (Ecast (Eunop Onotbool (Etempvar _t'5 tint) tint) tbool)))))
    (Sifthenelse (Etempvar _t'4 tint)
      (Ssequence
        (Scall (Some _t'1)
          (Evar _fail_runqueue (Tfunction
                                 (tint ::
                                  (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                  (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                  nil) tint cc_default))
          ((Econst_int (Int.repr 1) tint) ::
           (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
           (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
           nil))
        (Sreturn (Some (Etempvar _t'1 tint))))
      Sskip))
  (Ssequence
    (Ssequence
      (Scall (Some _t'10)
        (Evar _find_entity_index (Tfunction
                                   ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                    tlong :: (tptr tulong) :: nil) tint
                                   cc_default))
        ((Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
         (Etempvar _thread_id tlong) ::
         (Eaddrof (Evar _existing tulong) (tptr tulong)) :: nil))
      (Sifthenelse (Etempvar _t'10 tint)
        (Ssequence
          (Ssequence
            (Sset _t'27 (Evar _existing tulong))
            (Ssequence
              (Sset _t'28
                (Efield
                  (Ederef
                    (Ebinop Oadd
                      (Efield
                        (Ederef
                          (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                          (Tstruct _pacha_eevdf_runqueue noattr)) _entities
                        (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                      (Etempvar _t'27 tulong)
                      (tptr (Tstruct _pacha_eevdf_entity noattr)))
                    (Tstruct _pacha_eevdf_entity noattr)) _state tint))
              (Sifthenelse (Ebinop One (Etempvar _t'28 tint)
                             (Econst_int (Int.repr 4) tint) tint)
                (Ssequence
                  (Scall (Some _t'6)
                    (Evar _fail_runqueue (Tfunction
                                           (tint ::
                                            (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                            (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                            nil) tint cc_default))
                    ((Econst_int (Int.repr 1) tint) ::
                     (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                     (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                     nil))
                  (Sreturn (Some (Etempvar _t'6 tint))))
                Sskip)))
          (Ssequence
            (Scall None
              (Evar _pacha_eevdf_empty_entity (Tfunction
                                                ((tptr (Tstruct _pacha_eevdf_entity noattr)) ::
                                                 nil) tvoid cc_default))
              ((Eaddrof (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
                 (tptr (Tstruct _pacha_eevdf_entity noattr))) :: nil))
            (Ssequence
              (Sassign
                (Efield (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
                  _thread_id tlong) (Etempvar _thread_id tlong))
              (Ssequence
                (Sassign
                  (Efield (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
                    _generation tlong) (Etempvar _generation tlong))
                (Ssequence
                  (Sassign
                    (Efield
                      (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
                      _weight tlong) (Etempvar _weight tlong))
                  (Ssequence
                    (Sassign
                      (Efield
                        (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
                        _slice_ns tlong) (Etempvar _slice_ns tlong))
                    (Ssequence
                      (Ssequence
                        (Sset _t'26
                          (Efield
                            (Ederef
                              (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                              (Tstruct _pacha_eevdf_runqueue noattr))
                            _min_vruntime tlong))
                        (Sassign
                          (Efield
                            (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
                            _vruntime tlong) (Etempvar _t'26 tlong)))
                      (Ssequence
                        (Ssequence
                          (Sset _t'25
                            (Efield
                              (Ederef
                                (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                                (Tstruct _pacha_eevdf_runqueue noattr))
                              _min_vruntime tlong))
                          (Sassign
                            (Efield
                              (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
                              _eligible_time tlong) (Etempvar _t'25 tlong)))
                        (Ssequence
                          (Ssequence
                            (Sset _t'24
                              (Efield
                                (Ederef
                                  (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                                  (Tstruct _pacha_eevdf_runqueue noattr))
                                _min_vruntime tlong))
                            (Sassign
                              (Efield
                                (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
                                _deadline tlong) (Etempvar _t'24 tlong)))
                          (Ssequence
                            (Sassign
                              (Efield
                                (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
                                _state tint) (Econst_int (Int.repr 1) tint))
                            (Ssequence
                              (Ssequence
                                (Ssequence
                                  (Sset _t'23
                                    (Efield
                                      (Ederef
                                        (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                                        (Tstruct _pacha_eevdf_runqueue noattr))
                                      _min_vruntime tlong))
                                  (Scall (Some _t'8)
                                    (Evar _refresh_deadline (Tfunction
                                                              ((tptr (Tstruct _pacha_eevdf_entity noattr)) ::
                                                               tlong ::
                                                               (tptr (Tstruct _pacha_eevdf_entity noattr)) ::
                                                               nil) tint
                                                              cc_default))
                                    ((Eaddrof
                                       (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
                                       (tptr (Tstruct _pacha_eevdf_entity noattr))) ::
                                     (Etempvar _t'23 tlong) ::
                                     (Eaddrof
                                       (Evar _refreshed (Tstruct _pacha_eevdf_entity noattr))
                                       (tptr (Tstruct _pacha_eevdf_entity noattr))) ::
                                     nil)))
                                (Sifthenelse (Eunop Onotbool
                                               (Etempvar _t'8 tint) tint)
                                  (Ssequence
                                    (Scall (Some _t'7)
                                      (Evar _fail_runqueue (Tfunction
                                                             (tint ::
                                                              (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                                              (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                                              nil) tint
                                                             cc_default))
                                      ((Econst_int (Int.repr 3) tint) ::
                                       (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                                       (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                                       nil))
                                    (Sreturn (Some (Etempvar _t'7 tint))))
                                  Sskip))
                              (Ssequence
                                (Sassign
                                  (Evar _next (Tstruct _pacha_eevdf_runqueue noattr))
                                  (Ederef
                                    (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                                    (Tstruct _pacha_eevdf_runqueue noattr)))
                                (Ssequence
                                  (Ssequence
                                    (Sset _t'22 (Evar _existing tulong))
                                    (Sassign
                                      (Ederef
                                        (Ebinop Oadd
                                          (Efield
                                            (Evar _next (Tstruct _pacha_eevdf_runqueue noattr))
                                            _entities
                                            (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                                          (Etempvar _t'22 tulong)
                                          (tptr (Tstruct _pacha_eevdf_entity noattr)))
                                        (Tstruct _pacha_eevdf_entity noattr))
                                      (Evar _refreshed (Tstruct _pacha_eevdf_entity noattr))))
                                  (Ssequence
                                    (Scall None
                                      (Evar _refresh_runqueue (Tfunction
                                                                ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                                                 nil) tvoid
                                                                cc_default))
                                      ((Eaddrof
                                         (Evar _next (Tstruct _pacha_eevdf_runqueue noattr))
                                         (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                                       nil))
                                    (Ssequence
                                      (Scall (Some _t'9)
                                        (Evar _ok_runqueue (Tfunction
                                                             ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                                              (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                                              nil) tint
                                                             cc_default))
                                        ((Eaddrof
                                           (Evar _next (Tstruct _pacha_eevdf_runqueue noattr))
                                           (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                                         (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                                         nil))
                                      (Sreturn (Some (Etempvar _t'9 tint))))))))))))))))))
        Sskip))
    (Ssequence
      (Ssequence
        (Sset _t'21
          (Efield
            (Ederef
              (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
              (Tstruct _pacha_eevdf_runqueue noattr)) _entity_count tulong))
        (Sifthenelse (Ebinop Oge (Etempvar _t'21 tulong)
                       (Econst_int (Int.repr 256) tuint) tint)
          (Ssequence
            (Scall (Some _t'11)
              (Evar _fail_runqueue (Tfunction
                                     (tint ::
                                      (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                      (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                      nil) tint cc_default))
              ((Econst_int (Int.repr 2) tint) ::
               (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
               (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
               nil))
            (Sreturn (Some (Etempvar _t'11 tint))))
          Sskip))
      (Ssequence
        (Scall None
          (Evar _pacha_eevdf_empty_entity (Tfunction
                                            ((tptr (Tstruct _pacha_eevdf_entity noattr)) ::
                                             nil) tvoid cc_default))
          ((Eaddrof (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
             (tptr (Tstruct _pacha_eevdf_entity noattr))) :: nil))
        (Ssequence
          (Sassign
            (Efield (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
              _thread_id tlong) (Etempvar _thread_id tlong))
          (Ssequence
            (Sassign
              (Efield (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
                _generation tlong) (Etempvar _generation tlong))
            (Ssequence
              (Sassign
                (Efield (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
                  _weight tlong) (Etempvar _weight tlong))
              (Ssequence
                (Sassign
                  (Efield (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
                    _slice_ns tlong) (Etempvar _slice_ns tlong))
                (Ssequence
                  (Ssequence
                    (Sset _t'20
                      (Efield
                        (Ederef
                          (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                          (Tstruct _pacha_eevdf_runqueue noattr))
                        _min_vruntime tlong))
                    (Sassign
                      (Efield
                        (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
                        _vruntime tlong) (Etempvar _t'20 tlong)))
                  (Ssequence
                    (Ssequence
                      (Sset _t'19
                        (Efield
                          (Ederef
                            (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                            (Tstruct _pacha_eevdf_runqueue noattr))
                          _min_vruntime tlong))
                      (Sassign
                        (Efield
                          (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
                          _eligible_time tlong) (Etempvar _t'19 tlong)))
                    (Ssequence
                      (Ssequence
                        (Sset _t'18
                          (Efield
                            (Ederef
                              (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                              (Tstruct _pacha_eevdf_runqueue noattr))
                            _min_vruntime tlong))
                        (Sassign
                          (Efield
                            (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
                            _deadline tlong) (Etempvar _t'18 tlong)))
                      (Ssequence
                        (Sassign
                          (Efield
                            (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
                            _state tint) (Econst_int (Int.repr 1) tint))
                        (Ssequence
                          (Ssequence
                            (Ssequence
                              (Sset _t'17
                                (Efield
                                  (Ederef
                                    (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                                    (Tstruct _pacha_eevdf_runqueue noattr))
                                  _min_vruntime tlong))
                              (Scall (Some _t'13)
                                (Evar _refresh_deadline (Tfunction
                                                          ((tptr (Tstruct _pacha_eevdf_entity noattr)) ::
                                                           tlong ::
                                                           (tptr (Tstruct _pacha_eevdf_entity noattr)) ::
                                                           nil) tint
                                                          cc_default))
                                ((Eaddrof
                                   (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
                                   (tptr (Tstruct _pacha_eevdf_entity noattr))) ::
                                 (Etempvar _t'17 tlong) ::
                                 (Eaddrof
                                   (Evar _refreshed (Tstruct _pacha_eevdf_entity noattr))
                                   (tptr (Tstruct _pacha_eevdf_entity noattr))) ::
                                 nil)))
                            (Sifthenelse (Eunop Onotbool
                                           (Etempvar _t'13 tint) tint)
                              (Ssequence
                                (Scall (Some _t'12)
                                  (Evar _fail_runqueue (Tfunction
                                                         (tint ::
                                                          (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                                          (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                                          nil) tint
                                                         cc_default))
                                  ((Econst_int (Int.repr 3) tint) ::
                                   (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                                   (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                                   nil))
                                (Sreturn (Some (Etempvar _t'12 tint))))
                              Sskip))
                          (Ssequence
                            (Sassign
                              (Evar _next__1 (Tstruct _pacha_eevdf_runqueue noattr))
                              (Ederef
                                (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                                (Tstruct _pacha_eevdf_runqueue noattr)))
                            (Ssequence
                              (Ssequence
                                (Sset _t'16
                                  (Efield
                                    (Evar _next__1 (Tstruct _pacha_eevdf_runqueue noattr))
                                    _entity_count tulong))
                                (Sassign
                                  (Ederef
                                    (Ebinop Oadd
                                      (Efield
                                        (Evar _next__1 (Tstruct _pacha_eevdf_runqueue noattr))
                                        _entities
                                        (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                                      (Etempvar _t'16 tulong)
                                      (tptr (Tstruct _pacha_eevdf_entity noattr)))
                                    (Tstruct _pacha_eevdf_entity noattr))
                                  (Evar _refreshed (Tstruct _pacha_eevdf_entity noattr))))
                              (Ssequence
                                (Ssequence
                                  (Sset _t'15
                                    (Efield
                                      (Evar _next__1 (Tstruct _pacha_eevdf_runqueue noattr))
                                      _entity_count tulong))
                                  (Sassign
                                    (Efield
                                      (Evar _next__1 (Tstruct _pacha_eevdf_runqueue noattr))
                                      _entity_count tulong)
                                    (Ebinop Oadd (Etempvar _t'15 tulong)
                                      (Econst_int (Int.repr 1) tint) tulong)))
                                (Ssequence
                                  (Scall None
                                    (Evar _refresh_runqueue (Tfunction
                                                              ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                                               nil) tvoid
                                                              cc_default))
                                    ((Eaddrof
                                       (Evar _next__1 (Tstruct _pacha_eevdf_runqueue noattr))
                                       (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                                     nil))
                                  (Ssequence
                                    (Scall (Some _t'14)
                                      (Evar _ok_runqueue (Tfunction
                                                           ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                                            (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                                            nil) tint
                                                           cc_default))
                                      ((Eaddrof
                                         (Evar _next__1 (Tstruct _pacha_eevdf_runqueue noattr))
                                         (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                                       (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                                       nil))
                                    (Sreturn (Some (Etempvar _t'14 tint)))))))))))))))))))))
|}.

Definition f_pacha_eevdf_wake := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_rq, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                (_thread_id, tlong) ::
                (_out, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) :: nil);
  fn_vars := ((_index, tulong) ::
              (_entity, (Tstruct _pacha_eevdf_entity noattr)) ::
              (_refreshed, (Tstruct _pacha_eevdf_entity noattr)) ::
              (_next, (Tstruct _pacha_eevdf_runqueue noattr)) :: nil);
  fn_temps := ((_t'6, tint) :: (_t'5, tint) :: (_t'4, tint) ::
               (_t'3, tint) :: (_t'2, tint) :: (_t'1, tint) ::
               (_t'11, tulong) :: (_t'10, tint) :: (_t'9, tlong) ::
               (_t'8, tlong) :: (_t'7, tulong) :: nil);
  fn_body :=
(Ssequence
  (Ssequence
    (Scall (Some _t'2)
      (Evar _find_entity_index (Tfunction
                                 ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                  tlong :: (tptr tulong) :: nil) tint
                                 cc_default))
      ((Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
       (Etempvar _thread_id tlong) ::
       (Eaddrof (Evar _index tulong) (tptr tulong)) :: nil))
    (Sifthenelse (Eunop Onotbool (Etempvar _t'2 tint) tint)
      (Ssequence
        (Scall (Some _t'1)
          (Evar _fail_runqueue (Tfunction
                                 (tint ::
                                  (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                  (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                  nil) tint cc_default))
          ((Econst_int (Int.repr 1) tint) ::
           (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
           (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
           nil))
        (Sreturn (Some (Etempvar _t'1 tint))))
      Sskip))
  (Ssequence
    (Ssequence
      (Sset _t'11 (Evar _index tulong))
      (Sassign (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
        (Ederef
          (Ebinop Oadd
            (Efield
              (Ederef
                (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                (Tstruct _pacha_eevdf_runqueue noattr)) _entities
              (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
            (Etempvar _t'11 tulong)
            (tptr (Tstruct _pacha_eevdf_entity noattr)))
          (Tstruct _pacha_eevdf_entity noattr))))
    (Ssequence
      (Ssequence
        (Sset _t'10
          (Efield (Evar _entity (Tstruct _pacha_eevdf_entity noattr)) _state
            tint))
        (Sifthenelse (Ebinop One (Etempvar _t'10 tint)
                       (Econst_int (Int.repr 3) tint) tint)
          (Ssequence
            (Scall (Some _t'3)
              (Evar _fail_runqueue (Tfunction
                                     (tint ::
                                      (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                      (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                      nil) tint cc_default))
              ((Econst_int (Int.repr 4) tint) ::
               (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
               (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
               nil))
            (Sreturn (Some (Etempvar _t'3 tint))))
          Sskip))
      (Ssequence
        (Ssequence
          (Sset _t'9
            (Efield
              (Ederef
                (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                (Tstruct _pacha_eevdf_runqueue noattr)) _min_vruntime tlong))
          (Scall None
            (Evar _place_entity_at_floor (Tfunction
                                           ((tptr (Tstruct _pacha_eevdf_entity noattr)) ::
                                            tlong :: nil) tvoid cc_default))
            ((Eaddrof (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
               (tptr (Tstruct _pacha_eevdf_entity noattr))) ::
             (Etempvar _t'9 tlong) :: nil)))
        (Ssequence
          (Scall None
            (Evar _set_entity_state (Tfunction
                                      ((tptr (Tstruct _pacha_eevdf_entity noattr)) ::
                                       tint :: nil) tvoid cc_default))
            ((Eaddrof (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
               (tptr (Tstruct _pacha_eevdf_entity noattr))) ::
             (Econst_int (Int.repr 1) tint) :: nil))
          (Ssequence
            (Ssequence
              (Ssequence
                (Sset _t'8
                  (Efield
                    (Ederef
                      (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                      (Tstruct _pacha_eevdf_runqueue noattr)) _min_vruntime
                    tlong))
                (Scall (Some _t'5)
                  (Evar _refresh_deadline (Tfunction
                                            ((tptr (Tstruct _pacha_eevdf_entity noattr)) ::
                                             tlong ::
                                             (tptr (Tstruct _pacha_eevdf_entity noattr)) ::
                                             nil) tint cc_default))
                  ((Eaddrof
                     (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
                     (tptr (Tstruct _pacha_eevdf_entity noattr))) ::
                   (Etempvar _t'8 tlong) ::
                   (Eaddrof
                     (Evar _refreshed (Tstruct _pacha_eevdf_entity noattr))
                     (tptr (Tstruct _pacha_eevdf_entity noattr))) :: nil)))
              (Sifthenelse (Eunop Onotbool (Etempvar _t'5 tint) tint)
                (Ssequence
                  (Scall (Some _t'4)
                    (Evar _fail_runqueue (Tfunction
                                           (tint ::
                                            (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                            (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                            nil) tint cc_default))
                    ((Econst_int (Int.repr 3) tint) ::
                     (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                     (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                     nil))
                  (Sreturn (Some (Etempvar _t'4 tint))))
                Sskip))
            (Ssequence
              (Sassign (Evar _next (Tstruct _pacha_eevdf_runqueue noattr))
                (Ederef
                  (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                  (Tstruct _pacha_eevdf_runqueue noattr)))
              (Ssequence
                (Ssequence
                  (Sset _t'7 (Evar _index tulong))
                  (Sassign
                    (Ederef
                      (Ebinop Oadd
                        (Efield
                          (Evar _next (Tstruct _pacha_eevdf_runqueue noattr))
                          _entities
                          (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                        (Etempvar _t'7 tulong)
                        (tptr (Tstruct _pacha_eevdf_entity noattr)))
                      (Tstruct _pacha_eevdf_entity noattr))
                    (Evar _refreshed (Tstruct _pacha_eevdf_entity noattr))))
                (Ssequence
                  (Scall None
                    (Evar _refresh_runqueue (Tfunction
                                              ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                               nil) tvoid cc_default))
                    ((Eaddrof
                       (Evar _next (Tstruct _pacha_eevdf_runqueue noattr))
                       (tptr (Tstruct _pacha_eevdf_runqueue noattr))) :: nil))
                  (Ssequence
                    (Scall (Some _t'6)
                      (Evar _ok_runqueue (Tfunction
                                           ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                            (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                            nil) tint cc_default))
                      ((Eaddrof
                         (Evar _next (Tstruct _pacha_eevdf_runqueue noattr))
                         (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                       (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                       nil))
                    (Sreturn (Some (Etempvar _t'6 tint)))))))))))))
|}.

Definition f_pacha_eevdf_block := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_rq, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                (_thread_id, tlong) ::
                (_out, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) :: nil);
  fn_vars := ((_index, tulong) ::
              (_entity, (Tstruct _pacha_eevdf_entity noattr)) ::
              (_next, (Tstruct _pacha_eevdf_runqueue noattr)) :: nil);
  fn_temps := ((_t'5, tint) :: (_t'4, tint) :: (_t'3, tint) ::
               (_t'2, tint) :: (_t'1, tint) :: (_t'7, tulong) ::
               (_t'6, tulong) :: nil);
  fn_body :=
(Ssequence
  (Ssequence
    (Scall (Some _t'2)
      (Evar _find_entity_index (Tfunction
                                 ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                  tlong :: (tptr tulong) :: nil) tint
                                 cc_default))
      ((Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
       (Etempvar _thread_id tlong) ::
       (Eaddrof (Evar _index tulong) (tptr tulong)) :: nil))
    (Sifthenelse (Eunop Onotbool (Etempvar _t'2 tint) tint)
      (Ssequence
        (Scall (Some _t'1)
          (Evar _fail_runqueue (Tfunction
                                 (tint ::
                                  (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                  (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                  nil) tint cc_default))
          ((Econst_int (Int.repr 1) tint) ::
           (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
           (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
           nil))
        (Sreturn (Some (Etempvar _t'1 tint))))
      Sskip))
  (Ssequence
    (Ssequence
      (Sset _t'7 (Evar _index tulong))
      (Sassign (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
        (Ederef
          (Ebinop Oadd
            (Efield
              (Ederef
                (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                (Tstruct _pacha_eevdf_runqueue noattr)) _entities
              (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
            (Etempvar _t'7 tulong)
            (tptr (Tstruct _pacha_eevdf_entity noattr)))
          (Tstruct _pacha_eevdf_entity noattr))))
    (Ssequence
      (Ssequence
        (Scall (Some _t'4)
          (Evar _runnable_or_running (Tfunction
                                       ((tptr (Tstruct _pacha_eevdf_entity noattr)) ::
                                        nil) tint cc_default))
          ((Eaddrof (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
             (tptr (Tstruct _pacha_eevdf_entity noattr))) :: nil))
        (Sifthenelse (Eunop Onotbool (Etempvar _t'4 tint) tint)
          (Ssequence
            (Scall (Some _t'3)
              (Evar _fail_runqueue (Tfunction
                                     (tint ::
                                      (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                      (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                      nil) tint cc_default))
              ((Econst_int (Int.repr 4) tint) ::
               (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
               (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
               nil))
            (Sreturn (Some (Etempvar _t'3 tint))))
          Sskip))
      (Ssequence
        (Sassign (Evar _next (Tstruct _pacha_eevdf_runqueue noattr))
          (Ederef
            (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
            (Tstruct _pacha_eevdf_runqueue noattr)))
        (Ssequence
          (Ssequence
            (Sset _t'6 (Evar _index tulong))
            (Scall None
              (Evar _set_entity_state (Tfunction
                                        ((tptr (Tstruct _pacha_eevdf_entity noattr)) ::
                                         tint :: nil) tvoid cc_default))
              ((Ebinop Oadd
                 (Efield (Evar _next (Tstruct _pacha_eevdf_runqueue noattr))
                   _entities
                   (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                 (Etempvar _t'6 tulong)
                 (tptr (Tstruct _pacha_eevdf_entity noattr))) ::
               (Econst_int (Int.repr 3) tint) :: nil)))
          (Ssequence
            (Scall None
              (Evar _refresh_runqueue (Tfunction
                                        ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                         nil) tvoid cc_default))
              ((Eaddrof (Evar _next (Tstruct _pacha_eevdf_runqueue noattr))
                 (tptr (Tstruct _pacha_eevdf_runqueue noattr))) :: nil))
            (Ssequence
              (Scall (Some _t'5)
                (Evar _ok_runqueue (Tfunction
                                     ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                      (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                      nil) tint cc_default))
                ((Eaddrof (Evar _next (Tstruct _pacha_eevdf_runqueue noattr))
                   (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                 (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                 nil))
              (Sreturn (Some (Etempvar _t'5 tint))))))))))
|}.

Definition f_pacha_eevdf_exit := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_rq, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                (_thread_id, tlong) ::
                (_out, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) :: nil);
  fn_vars := ((_index, tulong) ::
              (_entity, (Tstruct _pacha_eevdf_entity noattr)) ::
              (_next, (Tstruct _pacha_eevdf_runqueue noattr)) :: nil);
  fn_temps := ((_t'5, tint) :: (_t'4, tint) :: (_t'3, tint) ::
               (_t'2, tint) :: (_t'1, tint) :: (_t'9, tulong) ::
               (_t'8, tint) :: (_t'7, tint) :: (_t'6, tulong) :: nil);
  fn_body :=
(Ssequence
  (Ssequence
    (Scall (Some _t'2)
      (Evar _find_entity_index (Tfunction
                                 ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                  tlong :: (tptr tulong) :: nil) tint
                                 cc_default))
      ((Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
       (Etempvar _thread_id tlong) ::
       (Eaddrof (Evar _index tulong) (tptr tulong)) :: nil))
    (Sifthenelse (Eunop Onotbool (Etempvar _t'2 tint) tint)
      (Ssequence
        (Scall (Some _t'1)
          (Evar _fail_runqueue (Tfunction
                                 (tint ::
                                  (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                  (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                  nil) tint cc_default))
          ((Econst_int (Int.repr 1) tint) ::
           (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
           (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
           nil))
        (Sreturn (Some (Etempvar _t'1 tint))))
      Sskip))
  (Ssequence
    (Ssequence
      (Sset _t'9 (Evar _index tulong))
      (Sassign (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
        (Ederef
          (Ebinop Oadd
            (Efield
              (Ederef
                (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                (Tstruct _pacha_eevdf_runqueue noattr)) _entities
              (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
            (Etempvar _t'9 tulong)
            (tptr (Tstruct _pacha_eevdf_entity noattr)))
          (Tstruct _pacha_eevdf_entity noattr))))
    (Ssequence
      (Ssequence
        (Ssequence
          (Sset _t'7
            (Efield (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
              _state tint))
          (Sifthenelse (Ebinop Oeq (Etempvar _t'7 tint)
                         (Econst_int (Int.repr 0) tint) tint)
            (Sset _t'4 (Econst_int (Int.repr 1) tint))
            (Ssequence
              (Sset _t'8
                (Efield (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
                  _state tint))
              (Sset _t'4
                (Ecast
                  (Ebinop Oeq (Etempvar _t'8 tint)
                    (Econst_int (Int.repr 4) tint) tint) tbool)))))
        (Sifthenelse (Etempvar _t'4 tint)
          (Ssequence
            (Scall (Some _t'3)
              (Evar _fail_runqueue (Tfunction
                                     (tint ::
                                      (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                      (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                      nil) tint cc_default))
              ((Econst_int (Int.repr 4) tint) ::
               (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
               (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
               nil))
            (Sreturn (Some (Etempvar _t'3 tint))))
          Sskip))
      (Ssequence
        (Sassign (Evar _next (Tstruct _pacha_eevdf_runqueue noattr))
          (Ederef
            (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
            (Tstruct _pacha_eevdf_runqueue noattr)))
        (Ssequence
          (Ssequence
            (Sset _t'6 (Evar _index tulong))
            (Scall None
              (Evar _set_entity_state (Tfunction
                                        ((tptr (Tstruct _pacha_eevdf_entity noattr)) ::
                                         tint :: nil) tvoid cc_default))
              ((Ebinop Oadd
                 (Efield (Evar _next (Tstruct _pacha_eevdf_runqueue noattr))
                   _entities
                   (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                 (Etempvar _t'6 tulong)
                 (tptr (Tstruct _pacha_eevdf_entity noattr))) ::
               (Econst_int (Int.repr 4) tint) :: nil)))
          (Ssequence
            (Scall None
              (Evar _refresh_runqueue (Tfunction
                                        ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                         nil) tvoid cc_default))
              ((Eaddrof (Evar _next (Tstruct _pacha_eevdf_runqueue noattr))
                 (tptr (Tstruct _pacha_eevdf_runqueue noattr))) :: nil))
            (Ssequence
              (Scall (Some _t'5)
                (Evar _ok_runqueue (Tfunction
                                     ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                      (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                      nil) tint cc_default))
                ((Eaddrof (Evar _next (Tstruct _pacha_eevdf_runqueue noattr))
                   (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                 (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                 nil))
              (Sreturn (Some (Etempvar _t'5 tint))))))))))
|}.

Definition f_pacha_eevdf_charge := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_rq, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                (_thread_id, tlong) :: (_runtime_ns, tlong) ::
                (_out, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) :: nil);
  fn_vars := ((_index, tulong) :: (_delta, tlong) :: (_vruntime, tlong) ::
              (_service_ns, tlong) ::
              (_entity, (Tstruct _pacha_eevdf_entity noattr)) ::
              (_refreshed, (Tstruct _pacha_eevdf_entity noattr)) ::
              (_next, (Tstruct _pacha_eevdf_runqueue noattr)) :: nil);
  fn_temps := ((_t'13, tint) :: (_t'12, tint) :: (_t'11, tint) ::
               (_t'10, tint) :: (_t'9, tint) :: (_t'8, tint) ::
               (_t'7, tint) :: (_t'6, tint) :: (_t'5, tint) ::
               (_t'4, tint) :: (_t'3, tint) :: (_t'2, tint) ::
               (_t'1, tint) :: (_t'22, tulong) :: (_t'21, tlong) ::
               (_t'20, tlong) :: (_t'19, tlong) :: (_t'18, tlong) ::
               (_t'17, tlong) :: (_t'16, tlong) :: (_t'15, tlong) ::
               (_t'14, tulong) :: nil);
  fn_body :=
(Ssequence
  (Ssequence
    (Scall (Some _t'2)
      (Evar _find_entity_index (Tfunction
                                 ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                  tlong :: (tptr tulong) :: nil) tint
                                 cc_default))
      ((Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
       (Etempvar _thread_id tlong) ::
       (Eaddrof (Evar _index tulong) (tptr tulong)) :: nil))
    (Sifthenelse (Eunop Onotbool (Etempvar _t'2 tint) tint)
      (Ssequence
        (Scall (Some _t'1)
          (Evar _fail_runqueue (Tfunction
                                 (tint ::
                                  (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                  (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                  nil) tint cc_default))
          ((Econst_int (Int.repr 1) tint) ::
           (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
           (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
           nil))
        (Sreturn (Some (Etempvar _t'1 tint))))
      Sskip))
  (Ssequence
    (Ssequence
      (Sset _t'22 (Evar _index tulong))
      (Sassign (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
        (Ederef
          (Ebinop Oadd
            (Efield
              (Ederef
                (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                (Tstruct _pacha_eevdf_runqueue noattr)) _entities
              (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
            (Etempvar _t'22 tulong)
            (tptr (Tstruct _pacha_eevdf_entity noattr)))
          (Tstruct _pacha_eevdf_entity noattr))))
    (Ssequence
      (Ssequence
        (Scall (Some _t'4)
          (Evar _runnable_or_running (Tfunction
                                       ((tptr (Tstruct _pacha_eevdf_entity noattr)) ::
                                        nil) tint cc_default))
          ((Eaddrof (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
             (tptr (Tstruct _pacha_eevdf_entity noattr))) :: nil))
        (Sifthenelse (Eunop Onotbool (Etempvar _t'4 tint) tint)
          (Ssequence
            (Scall (Some _t'3)
              (Evar _fail_runqueue (Tfunction
                                     (tint ::
                                      (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                      (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                      nil) tint cc_default))
              ((Econst_int (Int.repr 4) tint) ::
               (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
               (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
               nil))
            (Sreturn (Some (Etempvar _t'3 tint))))
          Sskip))
      (Ssequence
        (Ssequence
          (Ssequence
            (Sset _t'21
              (Efield (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
                _weight tlong))
            (Scall (Some _t'6)
              (Evar _weighted_delta (Tfunction
                                      (tlong :: tlong :: (tptr tlong) :: nil)
                                      tint cc_default))
              ((Etempvar _runtime_ns tlong) :: (Etempvar _t'21 tlong) ::
               (Eaddrof (Evar _delta tlong) (tptr tlong)) :: nil)))
          (Sifthenelse (Eunop Onotbool (Etempvar _t'6 tint) tint)
            (Ssequence
              (Scall (Some _t'5)
                (Evar _fail_runqueue (Tfunction
                                       (tint ::
                                        (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                        (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                        nil) tint cc_default))
                ((Econst_int (Int.repr 3) tint) ::
                 (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                 (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                 nil))
              (Sreturn (Some (Etempvar _t'5 tint))))
            Sskip))
        (Ssequence
          (Ssequence
            (Ssequence
              (Sset _t'19
                (Efield (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
                  _vruntime tlong))
              (Ssequence
                (Sset _t'20 (Evar _delta tlong))
                (Scall (Some _t'8)
                  (Evar _checked_add_i64 (Tfunction
                                           (tlong :: tlong :: (tptr tlong) ::
                                            nil) tint cc_default))
                  ((Etempvar _t'19 tlong) :: (Etempvar _t'20 tlong) ::
                   (Eaddrof (Evar _vruntime tlong) (tptr tlong)) :: nil))))
            (Sifthenelse (Eunop Onotbool (Etempvar _t'8 tint) tint)
              (Ssequence
                (Scall (Some _t'7)
                  (Evar _fail_runqueue (Tfunction
                                         (tint ::
                                          (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                          (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                          nil) tint cc_default))
                  ((Econst_int (Int.repr 3) tint) ::
                   (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                   (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                   nil))
                (Sreturn (Some (Etempvar _t'7 tint))))
              Sskip))
          (Ssequence
            (Ssequence
              (Ssequence
                (Sset _t'18
                  (Efield (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
                    _service_ns tlong))
                (Scall (Some _t'10)
                  (Evar _checked_add_i64 (Tfunction
                                           (tlong :: tlong :: (tptr tlong) ::
                                            nil) tint cc_default))
                  ((Etempvar _t'18 tlong) :: (Etempvar _runtime_ns tlong) ::
                   (Eaddrof (Evar _service_ns tlong) (tptr tlong)) :: nil)))
              (Sifthenelse (Eunop Onotbool (Etempvar _t'10 tint) tint)
                (Ssequence
                  (Scall (Some _t'9)
                    (Evar _fail_runqueue (Tfunction
                                           (tint ::
                                            (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                            (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                            nil) tint cc_default))
                    ((Econst_int (Int.repr 3) tint) ::
                     (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                     (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                     nil))
                  (Sreturn (Some (Etempvar _t'9 tint))))
                Sskip))
            (Ssequence
              (Ssequence
                (Sset _t'17 (Evar _service_ns tlong))
                (Sassign
                  (Efield (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
                    _service_ns tlong) (Etempvar _t'17 tlong)))
              (Ssequence
                (Ssequence
                  (Sset _t'16 (Evar _vruntime tlong))
                  (Sassign
                    (Efield
                      (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
                      _vruntime tlong) (Etempvar _t'16 tlong)))
                (Ssequence
                  (Ssequence
                    (Ssequence
                      (Sset _t'15
                        (Efield
                          (Ederef
                            (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                            (Tstruct _pacha_eevdf_runqueue noattr))
                          _min_vruntime tlong))
                      (Scall (Some _t'12)
                        (Evar _refresh_deadline (Tfunction
                                                  ((tptr (Tstruct _pacha_eevdf_entity noattr)) ::
                                                   tlong ::
                                                   (tptr (Tstruct _pacha_eevdf_entity noattr)) ::
                                                   nil) tint cc_default))
                        ((Eaddrof
                           (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
                           (tptr (Tstruct _pacha_eevdf_entity noattr))) ::
                         (Etempvar _t'15 tlong) ::
                         (Eaddrof
                           (Evar _refreshed (Tstruct _pacha_eevdf_entity noattr))
                           (tptr (Tstruct _pacha_eevdf_entity noattr))) ::
                         nil)))
                    (Sifthenelse (Eunop Onotbool (Etempvar _t'12 tint) tint)
                      (Ssequence
                        (Scall (Some _t'11)
                          (Evar _fail_runqueue (Tfunction
                                                 (tint ::
                                                  (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                                  (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                                  nil) tint cc_default))
                          ((Econst_int (Int.repr 3) tint) ::
                           (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                           (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                           nil))
                        (Sreturn (Some (Etempvar _t'11 tint))))
                      Sskip))
                  (Ssequence
                    (Sassign
                      (Evar _next (Tstruct _pacha_eevdf_runqueue noattr))
                      (Ederef
                        (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                        (Tstruct _pacha_eevdf_runqueue noattr)))
                    (Ssequence
                      (Ssequence
                        (Sset _t'14 (Evar _index tulong))
                        (Sassign
                          (Ederef
                            (Ebinop Oadd
                              (Efield
                                (Evar _next (Tstruct _pacha_eevdf_runqueue noattr))
                                _entities
                                (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                              (Etempvar _t'14 tulong)
                              (tptr (Tstruct _pacha_eevdf_entity noattr)))
                            (Tstruct _pacha_eevdf_entity noattr))
                          (Evar _refreshed (Tstruct _pacha_eevdf_entity noattr))))
                      (Ssequence
                        (Scall None
                          (Evar _refresh_runqueue (Tfunction
                                                    ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                                     nil) tvoid cc_default))
                          ((Eaddrof
                             (Evar _next (Tstruct _pacha_eevdf_runqueue noattr))
                             (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                           nil))
                        (Ssequence
                          (Scall (Some _t'13)
                            (Evar _ok_runqueue (Tfunction
                                                 ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                                  (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                                  nil) tint cc_default))
                            ((Eaddrof
                               (Evar _next (Tstruct _pacha_eevdf_runqueue noattr))
                               (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                             (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                             nil))
                          (Sreturn (Some (Etempvar _t'13 tint))))))))))))))))
|}.

Definition f_pacha_eevdf_mark_running := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_rq, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                (_thread_id, tlong) ::
                (_out, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) :: nil);
  fn_vars := ((_index, tulong) ::
              (_entity, (Tstruct _pacha_eevdf_entity noattr)) ::
              (_next, (Tstruct _pacha_eevdf_runqueue noattr)) :: nil);
  fn_temps := ((_t'4, tint) :: (_t'3, tint) :: (_t'2, tint) ::
               (_t'1, tint) :: (_t'7, tulong) :: (_t'6, tint) ::
               (_t'5, tulong) :: nil);
  fn_body :=
(Ssequence
  (Ssequence
    (Scall (Some _t'2)
      (Evar _find_entity_index (Tfunction
                                 ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                  tlong :: (tptr tulong) :: nil) tint
                                 cc_default))
      ((Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
       (Etempvar _thread_id tlong) ::
       (Eaddrof (Evar _index tulong) (tptr tulong)) :: nil))
    (Sifthenelse (Eunop Onotbool (Etempvar _t'2 tint) tint)
      (Ssequence
        (Scall (Some _t'1)
          (Evar _fail_runqueue (Tfunction
                                 (tint ::
                                  (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                  (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                  nil) tint cc_default))
          ((Econst_int (Int.repr 1) tint) ::
           (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
           (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
           nil))
        (Sreturn (Some (Etempvar _t'1 tint))))
      Sskip))
  (Ssequence
    (Ssequence
      (Sset _t'7 (Evar _index tulong))
      (Sassign (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
        (Ederef
          (Ebinop Oadd
            (Efield
              (Ederef
                (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                (Tstruct _pacha_eevdf_runqueue noattr)) _entities
              (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
            (Etempvar _t'7 tulong)
            (tptr (Tstruct _pacha_eevdf_entity noattr)))
          (Tstruct _pacha_eevdf_entity noattr))))
    (Ssequence
      (Ssequence
        (Sset _t'6
          (Efield (Evar _entity (Tstruct _pacha_eevdf_entity noattr)) _state
            tint))
        (Sifthenelse (Ebinop One (Etempvar _t'6 tint)
                       (Econst_int (Int.repr 1) tint) tint)
          (Ssequence
            (Scall (Some _t'3)
              (Evar _fail_runqueue (Tfunction
                                     (tint ::
                                      (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                      (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                      nil) tint cc_default))
              ((Econst_int (Int.repr 4) tint) ::
               (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
               (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
               nil))
            (Sreturn (Some (Etempvar _t'3 tint))))
          Sskip))
      (Ssequence
        (Sassign (Evar _next (Tstruct _pacha_eevdf_runqueue noattr))
          (Ederef
            (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
            (Tstruct _pacha_eevdf_runqueue noattr)))
        (Ssequence
          (Ssequence
            (Sset _t'5 (Evar _index tulong))
            (Scall None
              (Evar _set_entity_state (Tfunction
                                        ((tptr (Tstruct _pacha_eevdf_entity noattr)) ::
                                         tint :: nil) tvoid cc_default))
              ((Ebinop Oadd
                 (Efield (Evar _next (Tstruct _pacha_eevdf_runqueue noattr))
                   _entities
                   (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                 (Etempvar _t'5 tulong)
                 (tptr (Tstruct _pacha_eevdf_entity noattr))) ::
               (Econst_int (Int.repr 2) tint) :: nil)))
          (Ssequence
            (Scall None
              (Evar _recount_runqueue (Tfunction
                                        ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                         nil) tvoid cc_default))
              ((Eaddrof (Evar _next (Tstruct _pacha_eevdf_runqueue noattr))
                 (tptr (Tstruct _pacha_eevdf_runqueue noattr))) :: nil))
            (Ssequence
              (Scall (Some _t'4)
                (Evar _ok_runqueue (Tfunction
                                     ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                      (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                      nil) tint cc_default))
                ((Eaddrof (Evar _next (Tstruct _pacha_eevdf_runqueue noattr))
                   (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                 (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                 nil))
              (Sreturn (Some (Etempvar _t'4 tint))))))))))
|}.

Definition f_pacha_eevdf_requeue_running := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_rq, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                (_thread_id, tlong) ::
                (_out, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) :: nil);
  fn_vars := ((_index, tulong) ::
              (_entity, (Tstruct _pacha_eevdf_entity noattr)) ::
              (_refreshed, (Tstruct _pacha_eevdf_entity noattr)) ::
              (_next, (Tstruct _pacha_eevdf_runqueue noattr)) :: nil);
  fn_temps := ((_t'6, tint) :: (_t'5, tint) :: (_t'4, tint) ::
               (_t'3, tint) :: (_t'2, tint) :: (_t'1, tint) ::
               (_t'10, tulong) :: (_t'9, tint) :: (_t'8, tlong) ::
               (_t'7, tulong) :: nil);
  fn_body :=
(Ssequence
  (Ssequence
    (Scall (Some _t'2)
      (Evar _find_entity_index (Tfunction
                                 ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                  tlong :: (tptr tulong) :: nil) tint
                                 cc_default))
      ((Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
       (Etempvar _thread_id tlong) ::
       (Eaddrof (Evar _index tulong) (tptr tulong)) :: nil))
    (Sifthenelse (Eunop Onotbool (Etempvar _t'2 tint) tint)
      (Ssequence
        (Scall (Some _t'1)
          (Evar _fail_runqueue (Tfunction
                                 (tint ::
                                  (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                  (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                  nil) tint cc_default))
          ((Econst_int (Int.repr 1) tint) ::
           (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
           (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
           nil))
        (Sreturn (Some (Etempvar _t'1 tint))))
      Sskip))
  (Ssequence
    (Ssequence
      (Sset _t'10 (Evar _index tulong))
      (Sassign (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
        (Ederef
          (Ebinop Oadd
            (Efield
              (Ederef
                (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                (Tstruct _pacha_eevdf_runqueue noattr)) _entities
              (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
            (Etempvar _t'10 tulong)
            (tptr (Tstruct _pacha_eevdf_entity noattr)))
          (Tstruct _pacha_eevdf_entity noattr))))
    (Ssequence
      (Ssequence
        (Sset _t'9
          (Efield (Evar _entity (Tstruct _pacha_eevdf_entity noattr)) _state
            tint))
        (Sifthenelse (Ebinop One (Etempvar _t'9 tint)
                       (Econst_int (Int.repr 2) tint) tint)
          (Ssequence
            (Scall (Some _t'3)
              (Evar _fail_runqueue (Tfunction
                                     (tint ::
                                      (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                      (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                      nil) tint cc_default))
              ((Econst_int (Int.repr 4) tint) ::
               (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
               (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
               nil))
            (Sreturn (Some (Etempvar _t'3 tint))))
          Sskip))
      (Ssequence
        (Scall None
          (Evar _set_entity_state (Tfunction
                                    ((tptr (Tstruct _pacha_eevdf_entity noattr)) ::
                                     tint :: nil) tvoid cc_default))
          ((Eaddrof (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
             (tptr (Tstruct _pacha_eevdf_entity noattr))) ::
           (Econst_int (Int.repr 1) tint) :: nil))
        (Ssequence
          (Ssequence
            (Ssequence
              (Sset _t'8
                (Efield
                  (Ederef
                    (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                    (Tstruct _pacha_eevdf_runqueue noattr)) _min_vruntime
                  tlong))
              (Scall (Some _t'5)
                (Evar _refresh_deadline (Tfunction
                                          ((tptr (Tstruct _pacha_eevdf_entity noattr)) ::
                                           tlong ::
                                           (tptr (Tstruct _pacha_eevdf_entity noattr)) ::
                                           nil) tint cc_default))
                ((Eaddrof (Evar _entity (Tstruct _pacha_eevdf_entity noattr))
                   (tptr (Tstruct _pacha_eevdf_entity noattr))) ::
                 (Etempvar _t'8 tlong) ::
                 (Eaddrof
                   (Evar _refreshed (Tstruct _pacha_eevdf_entity noattr))
                   (tptr (Tstruct _pacha_eevdf_entity noattr))) :: nil)))
            (Sifthenelse (Eunop Onotbool (Etempvar _t'5 tint) tint)
              (Ssequence
                (Scall (Some _t'4)
                  (Evar _fail_runqueue (Tfunction
                                         (tint ::
                                          (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                          (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                          nil) tint cc_default))
                  ((Econst_int (Int.repr 3) tint) ::
                   (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                   (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                   nil))
                (Sreturn (Some (Etempvar _t'4 tint))))
              Sskip))
          (Ssequence
            (Sassign (Evar _next (Tstruct _pacha_eevdf_runqueue noattr))
              (Ederef
                (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                (Tstruct _pacha_eevdf_runqueue noattr)))
            (Ssequence
              (Ssequence
                (Sset _t'7 (Evar _index tulong))
                (Sassign
                  (Ederef
                    (Ebinop Oadd
                      (Efield
                        (Evar _next (Tstruct _pacha_eevdf_runqueue noattr))
                        _entities
                        (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                      (Etempvar _t'7 tulong)
                      (tptr (Tstruct _pacha_eevdf_entity noattr)))
                    (Tstruct _pacha_eevdf_entity noattr))
                  (Evar _refreshed (Tstruct _pacha_eevdf_entity noattr))))
              (Ssequence
                (Scall None
                  (Evar _refresh_runqueue (Tfunction
                                            ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                             nil) tvoid cc_default))
                  ((Eaddrof
                     (Evar _next (Tstruct _pacha_eevdf_runqueue noattr))
                     (tptr (Tstruct _pacha_eevdf_runqueue noattr))) :: nil))
                (Ssequence
                  (Scall (Some _t'6)
                    (Evar _ok_runqueue (Tfunction
                                         ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                          (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                          nil) tint cc_default))
                    ((Eaddrof
                       (Evar _next (Tstruct _pacha_eevdf_runqueue noattr))
                       (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                     (Etempvar _out (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                     nil))
                  (Sreturn (Some (Etempvar _t'6 tint))))))))))))
|}.

Definition f_pacha_eevdf_pick := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_rq, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                (_out, (tptr (Tstruct _pacha_eevdf_pick_result noattr))) ::
                nil);
  fn_vars := nil;
  fn_temps := ((_best_index, tulong) :: (_have_best, tint) :: (_i, tulong) ::
               (_entity, (tptr (Tstruct _pacha_eevdf_entity noattr))) ::
               (_have_next, tint) :: (_next_time, tlong) ::
               (_i__1, tulong) ::
               (_entity__1, (tptr (Tstruct _pacha_eevdf_entity noattr))) ::
               (_i__2, tulong) ::
               (_entity__2, (tptr (Tstruct _pacha_eevdf_entity noattr))) ::
               (_t'10, tint) :: (_t'9, tint) :: (_t'8, tint) ::
               (_t'7, tint) :: (_t'6, tint) :: (_t'5, tint) ::
               (_t'4, tint) :: (_t'3, tint) :: (_t'2, tint) ::
               (_t'1, tint) :: (_t'18, tulong) :: (_t'17, tlong) ::
               (_t'16, tlong) :: (_t'15, tulong) :: (_t'14, tlong) ::
               (_t'13, tulong) :: (_t'12, tlong) :: (_t'11, tint) :: nil);
  fn_body :=
(Ssequence
  (Sassign
    (Efield
      (Ederef
        (Etempvar _out (tptr (Tstruct _pacha_eevdf_pick_result noattr)))
        (Tstruct _pacha_eevdf_pick_result noattr)) _rq
      (Tstruct _pacha_eevdf_runqueue noattr))
    (Ederef (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
      (Tstruct _pacha_eevdf_runqueue noattr)))
  (Ssequence
    (Sassign
      (Efield
        (Ederef
          (Etempvar _out (tptr (Tstruct _pacha_eevdf_pick_result noattr)))
          (Tstruct _pacha_eevdf_pick_result noattr)) _has_entity tint)
      (Econst_int (Int.repr 0) tint))
    (Ssequence
      (Sassign
        (Efield
          (Ederef
            (Etempvar _out (tptr (Tstruct _pacha_eevdf_pick_result noattr)))
            (Tstruct _pacha_eevdf_pick_result noattr)) _index tulong)
        (Econst_int (Int.repr 0) tint))
      (Ssequence
        (Scall None
          (Evar _pacha_eevdf_empty_entity (Tfunction
                                            ((tptr (Tstruct _pacha_eevdf_entity noattr)) ::
                                             nil) tvoid cc_default))
          ((Eaddrof
             (Efield
               (Ederef
                 (Etempvar _out (tptr (Tstruct _pacha_eevdf_pick_result noattr)))
                 (Tstruct _pacha_eevdf_pick_result noattr)) _entity
               (Tstruct _pacha_eevdf_entity noattr))
             (tptr (Tstruct _pacha_eevdf_entity noattr))) :: nil))
        (Ssequence
          (Sset _best_index (Ecast (Econst_int (Int.repr 0) tint) tulong))
          (Ssequence
            (Sset _have_best (Econst_int (Int.repr 0) tint))
            (Ssequence
              (Ssequence
                (Sset _i (Ecast (Econst_int (Int.repr 0) tint) tulong))
                (Sloop
                  (Ssequence
                    (Ssequence
                      (Sset _t'18
                        (Efield
                          (Ederef
                            (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                            (Tstruct _pacha_eevdf_runqueue noattr))
                          _entity_count tulong))
                      (Sifthenelse (Ebinop Olt (Etempvar _i tulong)
                                     (Etempvar _t'18 tulong) tint)
                        Sskip
                        Sbreak))
                    (Ssequence
                      (Sset _entity
                        (Ebinop Oadd
                          (Efield
                            (Ederef
                              (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                              (Tstruct _pacha_eevdf_runqueue noattr))
                            _entities
                            (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                          (Etempvar _i tulong)
                          (tptr (Tstruct _pacha_eevdf_entity noattr))))
                      (Ssequence
                        (Ssequence
                          (Ssequence
                            (Scall (Some _t'1)
                              (Evar _is_runnable (Tfunction
                                                   ((tptr (Tstruct _pacha_eevdf_entity noattr)) ::
                                                    nil) tint cc_default))
                              ((Etempvar _entity (tptr (Tstruct _pacha_eevdf_entity noattr))) ::
                               nil))
                            (Sifthenelse (Eunop Onotbool (Etempvar _t'1 tint)
                                           tint)
                              (Sset _t'2 (Econst_int (Int.repr 1) tint))
                              (Ssequence
                                (Sset _t'16
                                  (Efield
                                    (Ederef
                                      (Etempvar _entity (tptr (Tstruct _pacha_eevdf_entity noattr)))
                                      (Tstruct _pacha_eevdf_entity noattr))
                                    _eligible_time tlong))
                                (Ssequence
                                  (Sset _t'17
                                    (Efield
                                      (Ederef
                                        (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                                        (Tstruct _pacha_eevdf_runqueue noattr))
                                      _virtual_time tlong))
                                  (Sset _t'2
                                    (Ecast
                                      (Ebinop Ogt (Etempvar _t'16 tlong)
                                        (Etempvar _t'17 tlong) tint) tbool))))))
                          (Sifthenelse (Etempvar _t'2 tint) Scontinue Sskip))
                        (Ssequence
                          (Sifthenelse (Eunop Onotbool
                                         (Etempvar _have_best tint) tint)
                            (Sset _t'3 (Econst_int (Int.repr 1) tint))
                            (Ssequence
                              (Scall (Some _t'4)
                                (Evar _entity_better (Tfunction
                                                       ((tptr (Tstruct _pacha_eevdf_entity noattr)) ::
                                                        (tptr (Tstruct _pacha_eevdf_entity noattr)) ::
                                                        nil) tint cc_default))
                                ((Etempvar _entity (tptr (Tstruct _pacha_eevdf_entity noattr))) ::
                                 (Eaddrof
                                   (Efield
                                     (Ederef
                                       (Etempvar _out (tptr (Tstruct _pacha_eevdf_pick_result noattr)))
                                       (Tstruct _pacha_eevdf_pick_result noattr))
                                     _entity
                                     (Tstruct _pacha_eevdf_entity noattr))
                                   (tptr (Tstruct _pacha_eevdf_entity noattr))) ::
                                 nil))
                              (Sset _t'3 (Ecast (Etempvar _t'4 tint) tbool))))
                          (Sifthenelse (Etempvar _t'3 tint)
                            (Ssequence
                              (Sassign
                                (Efield
                                  (Ederef
                                    (Etempvar _out (tptr (Tstruct _pacha_eevdf_pick_result noattr)))
                                    (Tstruct _pacha_eevdf_pick_result noattr))
                                  _entity
                                  (Tstruct _pacha_eevdf_entity noattr))
                                (Ederef
                                  (Etempvar _entity (tptr (Tstruct _pacha_eevdf_entity noattr)))
                                  (Tstruct _pacha_eevdf_entity noattr)))
                              (Ssequence
                                (Sset _best_index (Etempvar _i tulong))
                                (Sset _have_best
                                  (Econst_int (Int.repr 1) tint))))
                            Sskip)))))
                  (Sset _i
                    (Ebinop Oadd (Etempvar _i tulong)
                      (Econst_int (Int.repr 1) tint) tulong))))
              (Ssequence
                (Sifthenelse (Etempvar _have_best tint)
                  (Ssequence
                    (Sassign
                      (Efield
                        (Ederef
                          (Etempvar _out (tptr (Tstruct _pacha_eevdf_pick_result noattr)))
                          (Tstruct _pacha_eevdf_pick_result noattr))
                        _has_entity tint) (Econst_int (Int.repr 1) tint))
                    (Ssequence
                      (Sassign
                        (Efield
                          (Ederef
                            (Etempvar _out (tptr (Tstruct _pacha_eevdf_pick_result noattr)))
                            (Tstruct _pacha_eevdf_pick_result noattr)) _index
                          tulong) (Etempvar _best_index tulong))
                      (Sreturn (Some (Econst_int (Int.repr 0) tint)))))
                  Sskip)
                (Ssequence
                  (Sset _have_next (Econst_int (Int.repr 0) tint))
                  (Ssequence
                    (Sset _next_time
                      (Ecast (Econst_int (Int.repr 0) tint) tlong))
                    (Ssequence
                      (Ssequence
                        (Sset _i__1
                          (Ecast (Econst_int (Int.repr 0) tint) tulong))
                        (Sloop
                          (Ssequence
                            (Ssequence
                              (Sset _t'15
                                (Efield
                                  (Ederef
                                    (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                                    (Tstruct _pacha_eevdf_runqueue noattr))
                                  _entity_count tulong))
                              (Sifthenelse (Ebinop Olt
                                             (Etempvar _i__1 tulong)
                                             (Etempvar _t'15 tulong) tint)
                                Sskip
                                Sbreak))
                            (Ssequence
                              (Sset _entity__1
                                (Ebinop Oadd
                                  (Efield
                                    (Ederef
                                      (Etempvar _rq (tptr (Tstruct _pacha_eevdf_runqueue noattr)))
                                      (Tstruct _pacha_eevdf_runqueue noattr))
                                    _entities
                                    (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                                  (Etempvar _i__1 tulong)
                                  (tptr (Tstruct _pacha_eevdf_entity noattr))))
                              (Ssequence
                                (Ssequence
                                  (Scall (Some _t'5)
                                    (Evar _is_runnable (Tfunction
                                                         ((tptr (Tstruct _pacha_eevdf_entity noattr)) ::
                                                          nil) tint
                                                         cc_default))
                                    ((Etempvar _entity__1 (tptr (Tstruct _pacha_eevdf_entity noattr))) ::
                                     nil))
                                  (Sifthenelse (Eunop Onotbool
                                                 (Etempvar _t'5 tint) tint)
                                    Scontinue
                                    Sskip))
                                (Ssequence
                                  (Sifthenelse (Eunop Onotbool
                                                 (Etempvar _have_next tint)
                                                 tint)
                                    (Sset _t'6
                                      (Econst_int (Int.repr 1) tint))
                                    (Ssequence
                                      (Sset _t'14
                                        (Efield
                                          (Ederef
                                            (Etempvar _entity__1 (tptr (Tstruct _pacha_eevdf_entity noattr)))
                                            (Tstruct _pacha_eevdf_entity noattr))
                                          _eligible_time tlong))
                                      (Sset _t'6
                                        (Ecast
                                          (Ebinop Olt (Etempvar _t'14 tlong)
                                            (Etempvar _next_time tlong) tint)
                                          tbool))))
                                  (Sifthenelse (Etempvar _t'6 tint)
                                    (Ssequence
                                      (Sset _next_time
                                        (Efield
                                          (Ederef
                                            (Etempvar _entity__1 (tptr (Tstruct _pacha_eevdf_entity noattr)))
                                            (Tstruct _pacha_eevdf_entity noattr))
                                          _eligible_time tlong))
                                      (Sset _have_next
                                        (Econst_int (Int.repr 1) tint)))
                                    Sskip)))))
                          (Sset _i__1
                            (Ebinop Oadd (Etempvar _i__1 tulong)
                              (Econst_int (Int.repr 1) tint) tulong))))
                      (Ssequence
                        (Sifthenelse (Eunop Onotbool
                                       (Etempvar _have_next tint) tint)
                          (Sreturn (Some (Econst_int (Int.repr 0) tint)))
                          Sskip)
                        (Ssequence
                          (Sassign
                            (Efield
                              (Efield
                                (Ederef
                                  (Etempvar _out (tptr (Tstruct _pacha_eevdf_pick_result noattr)))
                                  (Tstruct _pacha_eevdf_pick_result noattr))
                                _rq (Tstruct _pacha_eevdf_runqueue noattr))
                              _virtual_time tlong)
                            (Etempvar _next_time tlong))
                          (Ssequence
                            (Ssequence
                              (Sset _i__2
                                (Ecast (Econst_int (Int.repr 0) tint) tulong))
                              (Sloop
                                (Ssequence
                                  (Ssequence
                                    (Sset _t'13
                                      (Efield
                                        (Efield
                                          (Ederef
                                            (Etempvar _out (tptr (Tstruct _pacha_eevdf_pick_result noattr)))
                                            (Tstruct _pacha_eevdf_pick_result noattr))
                                          _rq
                                          (Tstruct _pacha_eevdf_runqueue noattr))
                                        _entity_count tulong))
                                    (Sifthenelse (Ebinop Olt
                                                   (Etempvar _i__2 tulong)
                                                   (Etempvar _t'13 tulong)
                                                   tint)
                                      Sskip
                                      Sbreak))
                                  (Ssequence
                                    (Sset _entity__2
                                      (Ebinop Oadd
                                        (Efield
                                          (Efield
                                            (Ederef
                                              (Etempvar _out (tptr (Tstruct _pacha_eevdf_pick_result noattr)))
                                              (Tstruct _pacha_eevdf_pick_result noattr))
                                            _rq
                                            (Tstruct _pacha_eevdf_runqueue noattr))
                                          _entities
                                          (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                                        (Etempvar _i__2 tulong)
                                        (tptr (Tstruct _pacha_eevdf_entity noattr))))
                                    (Ssequence
                                      (Ssequence
                                        (Ssequence
                                          (Scall (Some _t'7)
                                            (Evar _is_runnable (Tfunction
                                                                 ((tptr (Tstruct _pacha_eevdf_entity noattr)) ::
                                                                  nil) tint
                                                                 cc_default))
                                            ((Etempvar _entity__2 (tptr (Tstruct _pacha_eevdf_entity noattr))) ::
                                             nil))
                                          (Sifthenelse (Eunop Onotbool
                                                         (Etempvar _t'7 tint)
                                                         tint)
                                            (Sset _t'8
                                              (Econst_int (Int.repr 1) tint))
                                            (Ssequence
                                              (Sset _t'12
                                                (Efield
                                                  (Ederef
                                                    (Etempvar _entity__2 (tptr (Tstruct _pacha_eevdf_entity noattr)))
                                                    (Tstruct _pacha_eevdf_entity noattr))
                                                  _eligible_time tlong))
                                              (Sset _t'8
                                                (Ecast
                                                  (Ebinop Ogt
                                                    (Etempvar _t'12 tlong)
                                                    (Etempvar _next_time tlong)
                                                    tint) tbool)))))
                                        (Sifthenelse (Etempvar _t'8 tint)
                                          Scontinue
                                          Sskip))
                                      (Ssequence
                                        (Ssequence
                                          (Sset _t'11
                                            (Efield
                                              (Ederef
                                                (Etempvar _out (tptr (Tstruct _pacha_eevdf_pick_result noattr)))
                                                (Tstruct _pacha_eevdf_pick_result noattr))
                                              _has_entity tint))
                                          (Sifthenelse (Eunop Onotbool
                                                         (Etempvar _t'11 tint)
                                                         tint)
                                            (Sset _t'9
                                              (Econst_int (Int.repr 1) tint))
                                            (Ssequence
                                              (Scall (Some _t'10)
                                                (Evar _entity_better 
                                                (Tfunction
                                                  ((tptr (Tstruct _pacha_eevdf_entity noattr)) ::
                                                   (tptr (Tstruct _pacha_eevdf_entity noattr)) ::
                                                   nil) tint cc_default))
                                                ((Etempvar _entity__2 (tptr (Tstruct _pacha_eevdf_entity noattr))) ::
                                                 (Eaddrof
                                                   (Efield
                                                     (Ederef
                                                       (Etempvar _out (tptr (Tstruct _pacha_eevdf_pick_result noattr)))
                                                       (Tstruct _pacha_eevdf_pick_result noattr))
                                                     _entity
                                                     (Tstruct _pacha_eevdf_entity noattr))
                                                   (tptr (Tstruct _pacha_eevdf_entity noattr))) ::
                                                 nil))
                                              (Sset _t'9
                                                (Ecast (Etempvar _t'10 tint)
                                                  tbool)))))
                                        (Sifthenelse (Etempvar _t'9 tint)
                                          (Ssequence
                                            (Sassign
                                              (Efield
                                                (Ederef
                                                  (Etempvar _out (tptr (Tstruct _pacha_eevdf_pick_result noattr)))
                                                  (Tstruct _pacha_eevdf_pick_result noattr))
                                                _entity
                                                (Tstruct _pacha_eevdf_entity noattr))
                                              (Ederef
                                                (Etempvar _entity__2 (tptr (Tstruct _pacha_eevdf_entity noattr)))
                                                (Tstruct _pacha_eevdf_entity noattr)))
                                            (Ssequence
                                              (Sassign
                                                (Efield
                                                  (Ederef
                                                    (Etempvar _out (tptr (Tstruct _pacha_eevdf_pick_result noattr)))
                                                    (Tstruct _pacha_eevdf_pick_result noattr))
                                                  _index tulong)
                                                (Etempvar _i__2 tulong))
                                              (Sassign
                                                (Efield
                                                  (Ederef
                                                    (Etempvar _out (tptr (Tstruct _pacha_eevdf_pick_result noattr)))
                                                    (Tstruct _pacha_eevdf_pick_result noattr))
                                                  _has_entity tint)
                                                (Econst_int (Int.repr 1) tint))))
                                          Sskip)))))
                                (Sset _i__2
                                  (Ebinop Oadd (Etempvar _i__2 tulong)
                                    (Econst_int (Int.repr 1) tint) tulong))))
                            (Sreturn (Some (Econst_int (Int.repr 0) tint)))))))))))))))))
|}.

Definition composites : list composite_definition :=
(Composite _pacha_eevdf_entity Struct
   (Member_plain _thread_id tlong :: Member_plain _generation tlong ::
    Member_plain _weight tlong :: Member_plain _slice_ns tlong ::
    Member_plain _service_ns tlong :: Member_plain _vruntime tlong ::
    Member_plain _eligible_time tlong :: Member_plain _deadline tlong ::
    Member_plain _state tint :: nil)
   noattr ::
 Composite _pacha_eevdf_runqueue Struct
   (Member_plain _entities (tarray (Tstruct _pacha_eevdf_entity noattr) 256) ::
    Member_plain _entity_count tulong ::
    Member_plain _runnable_count tulong ::
    Member_plain _virtual_time tlong :: Member_plain _min_vruntime tlong ::
    nil)
   noattr ::
 Composite _pacha_eevdf_pick_result Struct
   (Member_plain _rq (Tstruct _pacha_eevdf_runqueue noattr) ::
    Member_plain _has_entity tint :: Member_plain _index tulong ::
    Member_plain _entity (Tstruct _pacha_eevdf_entity noattr) :: nil)
   noattr :: nil).

Definition global_definitions : list (ident * globdef fundef type) :=
((___compcert_va_int32,
   Gfun(External (EF_runtime "__compcert_va_int32"
                   (mksignature (AST.Xptr :: nil) AST.Xint cc_default))
     ((tptr tvoid) :: nil) tuint cc_default)) ::
 (___compcert_va_int64,
   Gfun(External (EF_runtime "__compcert_va_int64"
                   (mksignature (AST.Xptr :: nil) AST.Xlong cc_default))
     ((tptr tvoid) :: nil) tulong cc_default)) ::
 (___compcert_va_float64,
   Gfun(External (EF_runtime "__compcert_va_float64"
                   (mksignature (AST.Xptr :: nil) AST.Xfloat cc_default))
     ((tptr tvoid) :: nil) tdouble cc_default)) ::
 (___compcert_va_composite,
   Gfun(External (EF_runtime "__compcert_va_composite"
                   (mksignature (AST.Xptr :: AST.Xlong :: nil) AST.Xptr
                     cc_default)) ((tptr tvoid) :: tulong :: nil)
     (tptr tvoid) cc_default)) ::
 (___compcert_i64_dtos,
   Gfun(External (EF_runtime "__compcert_i64_dtos"
                   (mksignature (AST.Xfloat :: nil) AST.Xlong cc_default))
     (tdouble :: nil) tlong cc_default)) ::
 (___compcert_i64_dtou,
   Gfun(External (EF_runtime "__compcert_i64_dtou"
                   (mksignature (AST.Xfloat :: nil) AST.Xlong cc_default))
     (tdouble :: nil) tulong cc_default)) ::
 (___compcert_i64_stod,
   Gfun(External (EF_runtime "__compcert_i64_stod"
                   (mksignature (AST.Xlong :: nil) AST.Xfloat cc_default))
     (tlong :: nil) tdouble cc_default)) ::
 (___compcert_i64_utod,
   Gfun(External (EF_runtime "__compcert_i64_utod"
                   (mksignature (AST.Xlong :: nil) AST.Xfloat cc_default))
     (tulong :: nil) tdouble cc_default)) ::
 (___compcert_i64_stof,
   Gfun(External (EF_runtime "__compcert_i64_stof"
                   (mksignature (AST.Xlong :: nil) AST.Xsingle cc_default))
     (tlong :: nil) tfloat cc_default)) ::
 (___compcert_i64_utof,
   Gfun(External (EF_runtime "__compcert_i64_utof"
                   (mksignature (AST.Xlong :: nil) AST.Xsingle cc_default))
     (tulong :: nil) tfloat cc_default)) ::
 (___compcert_i64_sdiv,
   Gfun(External (EF_runtime "__compcert_i64_sdiv"
                   (mksignature (AST.Xlong :: AST.Xlong :: nil) AST.Xlong
                     cc_default)) (tlong :: tlong :: nil) tlong cc_default)) ::
 (___compcert_i64_udiv,
   Gfun(External (EF_runtime "__compcert_i64_udiv"
                   (mksignature (AST.Xlong :: AST.Xlong :: nil) AST.Xlong
                     cc_default)) (tulong :: tulong :: nil) tulong
     cc_default)) ::
 (___compcert_i64_smod,
   Gfun(External (EF_runtime "__compcert_i64_smod"
                   (mksignature (AST.Xlong :: AST.Xlong :: nil) AST.Xlong
                     cc_default)) (tlong :: tlong :: nil) tlong cc_default)) ::
 (___compcert_i64_umod,
   Gfun(External (EF_runtime "__compcert_i64_umod"
                   (mksignature (AST.Xlong :: AST.Xlong :: nil) AST.Xlong
                     cc_default)) (tulong :: tulong :: nil) tulong
     cc_default)) ::
 (___compcert_i64_shl,
   Gfun(External (EF_runtime "__compcert_i64_shl"
                   (mksignature (AST.Xlong :: AST.Xint :: nil) AST.Xlong
                     cc_default)) (tlong :: tint :: nil) tlong cc_default)) ::
 (___compcert_i64_shr,
   Gfun(External (EF_runtime "__compcert_i64_shr"
                   (mksignature (AST.Xlong :: AST.Xint :: nil) AST.Xlong
                     cc_default)) (tulong :: tint :: nil) tulong cc_default)) ::
 (___compcert_i64_sar,
   Gfun(External (EF_runtime "__compcert_i64_sar"
                   (mksignature (AST.Xlong :: AST.Xint :: nil) AST.Xlong
                     cc_default)) (tlong :: tint :: nil) tlong cc_default)) ::
 (___compcert_i64_smulh,
   Gfun(External (EF_runtime "__compcert_i64_smulh"
                   (mksignature (AST.Xlong :: AST.Xlong :: nil) AST.Xlong
                     cc_default)) (tlong :: tlong :: nil) tlong cc_default)) ::
 (___compcert_i64_umulh,
   Gfun(External (EF_runtime "__compcert_i64_umulh"
                   (mksignature (AST.Xlong :: AST.Xlong :: nil) AST.Xlong
                     cc_default)) (tulong :: tulong :: nil) tulong
     cc_default)) ::
 (___builtin_ais_annot,
   Gfun(External (EF_builtin "__builtin_ais_annot"
                   (mksignature (AST.Xptr :: nil) AST.Xvoid
                     {|cc_vararg:=(Some 1); cc_unproto:=false; cc_structret:=false|}))
     ((tptr tschar) :: nil) tvoid
     {|cc_vararg:=(Some 1); cc_unproto:=false; cc_structret:=false|})) ::
 (___builtin_bswap64,
   Gfun(External (EF_builtin "__builtin_bswap64"
                   (mksignature (AST.Xlong :: nil) AST.Xlong cc_default))
     (tulong :: nil) tulong cc_default)) ::
 (___builtin_bswap,
   Gfun(External (EF_builtin "__builtin_bswap"
                   (mksignature (AST.Xint :: nil) AST.Xint cc_default))
     (tuint :: nil) tuint cc_default)) ::
 (___builtin_bswap32,
   Gfun(External (EF_builtin "__builtin_bswap32"
                   (mksignature (AST.Xint :: nil) AST.Xint cc_default))
     (tuint :: nil) tuint cc_default)) ::
 (___builtin_bswap16,
   Gfun(External (EF_builtin "__builtin_bswap16"
                   (mksignature (AST.Xint16unsigned :: nil)
                     AST.Xint16unsigned cc_default)) (tushort :: nil) tushort
     cc_default)) ::
 (___builtin_clz,
   Gfun(External (EF_builtin "__builtin_clz"
                   (mksignature (AST.Xint :: nil) AST.Xint cc_default))
     (tuint :: nil) tint cc_default)) ::
 (___builtin_clzl,
   Gfun(External (EF_builtin "__builtin_clzl"
                   (mksignature (AST.Xlong :: nil) AST.Xint cc_default))
     (tulong :: nil) tint cc_default)) ::
 (___builtin_clzll,
   Gfun(External (EF_builtin "__builtin_clzll"
                   (mksignature (AST.Xlong :: nil) AST.Xint cc_default))
     (tulong :: nil) tint cc_default)) ::
 (___builtin_ctz,
   Gfun(External (EF_builtin "__builtin_ctz"
                   (mksignature (AST.Xint :: nil) AST.Xint cc_default))
     (tuint :: nil) tint cc_default)) ::
 (___builtin_ctzl,
   Gfun(External (EF_builtin "__builtin_ctzl"
                   (mksignature (AST.Xlong :: nil) AST.Xint cc_default))
     (tulong :: nil) tint cc_default)) ::
 (___builtin_ctzll,
   Gfun(External (EF_builtin "__builtin_ctzll"
                   (mksignature (AST.Xlong :: nil) AST.Xint cc_default))
     (tulong :: nil) tint cc_default)) ::
 (___builtin_fabs,
   Gfun(External (EF_builtin "__builtin_fabs"
                   (mksignature (AST.Xfloat :: nil) AST.Xfloat cc_default))
     (tdouble :: nil) tdouble cc_default)) ::
 (___builtin_fabsf,
   Gfun(External (EF_builtin "__builtin_fabsf"
                   (mksignature (AST.Xsingle :: nil) AST.Xsingle cc_default))
     (tfloat :: nil) tfloat cc_default)) ::
 (___builtin_fsqrt,
   Gfun(External (EF_builtin "__builtin_fsqrt"
                   (mksignature (AST.Xfloat :: nil) AST.Xfloat cc_default))
     (tdouble :: nil) tdouble cc_default)) ::
 (___builtin_sqrt,
   Gfun(External (EF_builtin "__builtin_sqrt"
                   (mksignature (AST.Xfloat :: nil) AST.Xfloat cc_default))
     (tdouble :: nil) tdouble cc_default)) ::
 (___builtin_memcpy_aligned,
   Gfun(External (EF_builtin "__builtin_memcpy_aligned"
                   (mksignature
                     (AST.Xptr :: AST.Xptr :: AST.Xlong :: AST.Xlong :: nil)
                     AST.Xvoid cc_default))
     ((tptr tvoid) :: (tptr tvoid) :: tulong :: tulong :: nil) tvoid
     cc_default)) ::
 (___builtin_sel,
   Gfun(External (EF_builtin "__builtin_sel"
                   (mksignature (AST.Xbool :: nil) AST.Xvoid
                     {|cc_vararg:=(Some 1); cc_unproto:=false; cc_structret:=false|}))
     (tbool :: nil) tvoid
     {|cc_vararg:=(Some 1); cc_unproto:=false; cc_structret:=false|})) ::
 (___builtin_annot,
   Gfun(External (EF_builtin "__builtin_annot"
                   (mksignature (AST.Xptr :: nil) AST.Xvoid
                     {|cc_vararg:=(Some 1); cc_unproto:=false; cc_structret:=false|}))
     ((tptr tschar) :: nil) tvoid
     {|cc_vararg:=(Some 1); cc_unproto:=false; cc_structret:=false|})) ::
 (___builtin_annot_intval,
   Gfun(External (EF_builtin "__builtin_annot_intval"
                   (mksignature (AST.Xptr :: AST.Xint :: nil) AST.Xint
                     cc_default)) ((tptr tschar) :: tint :: nil) tint
     cc_default)) ::
 (___builtin_membar,
   Gfun(External (EF_builtin "__builtin_membar"
                   (mksignature nil AST.Xvoid cc_default)) nil tvoid
     cc_default)) ::
 (___builtin_va_start,
   Gfun(External (EF_builtin "__builtin_va_start"
                   (mksignature (AST.Xptr :: nil) AST.Xvoid cc_default))
     ((tptr tvoid) :: nil) tvoid cc_default)) ::
 (___builtin_va_arg,
   Gfun(External (EF_builtin "__builtin_va_arg"
                   (mksignature (AST.Xptr :: AST.Xint :: nil) AST.Xvoid
                     cc_default)) ((tptr tvoid) :: tuint :: nil) tvoid
     cc_default)) ::
 (___builtin_va_copy,
   Gfun(External (EF_builtin "__builtin_va_copy"
                   (mksignature (AST.Xptr :: AST.Xptr :: nil) AST.Xvoid
                     cc_default)) ((tptr tvoid) :: (tptr tvoid) :: nil) tvoid
     cc_default)) ::
 (___builtin_va_end,
   Gfun(External (EF_builtin "__builtin_va_end"
                   (mksignature (AST.Xptr :: nil) AST.Xvoid cc_default))
     ((tptr tvoid) :: nil) tvoid cc_default)) ::
 (___builtin_unreachable,
   Gfun(External (EF_builtin "__builtin_unreachable"
                   (mksignature nil AST.Xvoid cc_default)) nil tvoid
     cc_default)) ::
 (___builtin_expect,
   Gfun(External (EF_builtin "__builtin_expect"
                   (mksignature (AST.Xlong :: AST.Xlong :: nil) AST.Xlong
                     cc_default)) (tlong :: tlong :: nil) tlong cc_default)) ::
 (___builtin_fmax,
   Gfun(External (EF_builtin "__builtin_fmax"
                   (mksignature (AST.Xfloat :: AST.Xfloat :: nil) AST.Xfloat
                     cc_default)) (tdouble :: tdouble :: nil) tdouble
     cc_default)) ::
 (___builtin_fmin,
   Gfun(External (EF_builtin "__builtin_fmin"
                   (mksignature (AST.Xfloat :: AST.Xfloat :: nil) AST.Xfloat
                     cc_default)) (tdouble :: tdouble :: nil) tdouble
     cc_default)) ::
 (___builtin_fmadd,
   Gfun(External (EF_builtin "__builtin_fmadd"
                   (mksignature
                     (AST.Xfloat :: AST.Xfloat :: AST.Xfloat :: nil)
                     AST.Xfloat cc_default))
     (tdouble :: tdouble :: tdouble :: nil) tdouble cc_default)) ::
 (___builtin_fmsub,
   Gfun(External (EF_builtin "__builtin_fmsub"
                   (mksignature
                     (AST.Xfloat :: AST.Xfloat :: AST.Xfloat :: nil)
                     AST.Xfloat cc_default))
     (tdouble :: tdouble :: tdouble :: nil) tdouble cc_default)) ::
 (___builtin_fnmadd,
   Gfun(External (EF_builtin "__builtin_fnmadd"
                   (mksignature
                     (AST.Xfloat :: AST.Xfloat :: AST.Xfloat :: nil)
                     AST.Xfloat cc_default))
     (tdouble :: tdouble :: tdouble :: nil) tdouble cc_default)) ::
 (___builtin_fnmsub,
   Gfun(External (EF_builtin "__builtin_fnmsub"
                   (mksignature
                     (AST.Xfloat :: AST.Xfloat :: AST.Xfloat :: nil)
                     AST.Xfloat cc_default))
     (tdouble :: tdouble :: tdouble :: nil) tdouble cc_default)) ::
 (___builtin_read16_reversed,
   Gfun(External (EF_builtin "__builtin_read16_reversed"
                   (mksignature (AST.Xptr :: nil) AST.Xint16unsigned
                     cc_default)) ((tptr tushort) :: nil) tushort
     cc_default)) ::
 (___builtin_read32_reversed,
   Gfun(External (EF_builtin "__builtin_read32_reversed"
                   (mksignature (AST.Xptr :: nil) AST.Xint cc_default))
     ((tptr tuint) :: nil) tuint cc_default)) ::
 (___builtin_write16_reversed,
   Gfun(External (EF_builtin "__builtin_write16_reversed"
                   (mksignature (AST.Xptr :: AST.Xint16unsigned :: nil)
                     AST.Xvoid cc_default))
     ((tptr tushort) :: tushort :: nil) tvoid cc_default)) ::
 (___builtin_write32_reversed,
   Gfun(External (EF_builtin "__builtin_write32_reversed"
                   (mksignature (AST.Xptr :: AST.Xint :: nil) AST.Xvoid
                     cc_default)) ((tptr tuint) :: tuint :: nil) tvoid
     cc_default)) ::
 (___builtin_debug,
   Gfun(External (EF_external "__builtin_debug"
                   (mksignature (AST.Xint :: nil) AST.Xvoid
                     {|cc_vararg:=(Some 1); cc_unproto:=false; cc_structret:=false|}))
     (tint :: nil) tvoid
     {|cc_vararg:=(Some 1); cc_unproto:=false; cc_structret:=false|})) ::
 (_valid_positive, Gfun(Internal f_valid_positive)) ::
 (_i64_nonnegative, Gfun(Internal f_i64_nonnegative)) ::
 (_i64_less, Gfun(Internal f_i64_less)) ::
 (_i64_equal, Gfun(Internal f_i64_equal)) ::
 (_checked_add_i64_overflows, Gfun(Internal f_checked_add_i64_overflows)) ::
 (_checked_add_i64, Gfun(Internal f_checked_add_i64)) ::
 (_checked_mul_i64_nonnegative_overflows, Gfun(Internal f_checked_mul_i64_nonnegative_overflows)) ::
 (_checked_mul_i64_nonnegative, Gfun(Internal f_checked_mul_i64_nonnegative)) ::
 (_z_max, Gfun(Internal f_z_max)) ::
 (_is_runnable, Gfun(Internal f_is_runnable)) ::
 (_live_for_min, Gfun(Internal f_live_for_min)) ::
 (_is_active_state, Gfun(Internal f_is_active_state)) ::
 (_runnable_or_running, Gfun(Internal f_runnable_or_running)) ::
 (_ok_runqueue, Gfun(Internal f_ok_runqueue)) ::
 (_fail_runqueue, Gfun(Internal f_fail_runqueue)) ::
 (_pacha_eevdf_copy_runqueue, Gfun(Internal f_pacha_eevdf_copy_runqueue)) ::
 (_count_runnable, Gfun(Internal f_count_runnable)) ::
 (_weighted_fractional_10, Gfun(Internal f_weighted_fractional_10)) ::
 (_weighted_delta, Gfun(Internal f_weighted_delta)) ::
 (_weighted_slice, Gfun(Internal f_weighted_slice)) ::
 (_refresh_deadline, Gfun(Internal f_refresh_deadline)) ::
 (_refresh_runqueue, Gfun(Internal f_refresh_runqueue)) ::
 (_recount_runqueue, Gfun(Internal f_recount_runqueue)) ::
 (_set_entity_state, Gfun(Internal f_set_entity_state)) ::
 (_place_entity_at_floor, Gfun(Internal f_place_entity_at_floor)) ::
 (_find_entity_index, Gfun(Internal f_find_entity_index)) ::
 (_entity_better_values, Gfun(Internal f_entity_better_values)) ::
 (_entity_better, Gfun(Internal f_entity_better)) ::
 (_pacha_eevdf_empty_entity, Gfun(Internal f_pacha_eevdf_empty_entity)) ::
 (_pacha_eevdf_empty_runqueue, Gfun(Internal f_pacha_eevdf_empty_runqueue)) ::
 (_pacha_eevdf_reset, Gfun(Internal f_pacha_eevdf_reset)) ::
 (_pacha_eevdf_add, Gfun(Internal f_pacha_eevdf_add)) ::
 (_pacha_eevdf_wake, Gfun(Internal f_pacha_eevdf_wake)) ::
 (_pacha_eevdf_block, Gfun(Internal f_pacha_eevdf_block)) ::
 (_pacha_eevdf_exit, Gfun(Internal f_pacha_eevdf_exit)) ::
 (_pacha_eevdf_charge, Gfun(Internal f_pacha_eevdf_charge)) ::
 (_pacha_eevdf_mark_running, Gfun(Internal f_pacha_eevdf_mark_running)) ::
 (_pacha_eevdf_requeue_running, Gfun(Internal f_pacha_eevdf_requeue_running)) ::
 (_pacha_eevdf_pick, Gfun(Internal f_pacha_eevdf_pick)) :: nil).

Definition public_idents : list ident :=
(_pacha_eevdf_pick :: _pacha_eevdf_requeue_running ::
 _pacha_eevdf_mark_running :: _pacha_eevdf_charge :: _pacha_eevdf_exit ::
 _pacha_eevdf_block :: _pacha_eevdf_wake :: _pacha_eevdf_add ::
 _pacha_eevdf_reset :: _pacha_eevdf_empty_runqueue ::
 _pacha_eevdf_empty_entity :: _pacha_eevdf_copy_runqueue ::
 ___builtin_debug :: ___builtin_write32_reversed ::
 ___builtin_write16_reversed :: ___builtin_read32_reversed ::
 ___builtin_read16_reversed :: ___builtin_fnmsub :: ___builtin_fnmadd ::
 ___builtin_fmsub :: ___builtin_fmadd :: ___builtin_fmin ::
 ___builtin_fmax :: ___builtin_expect :: ___builtin_unreachable ::
 ___builtin_va_end :: ___builtin_va_copy :: ___builtin_va_arg ::
 ___builtin_va_start :: ___builtin_membar :: ___builtin_annot_intval ::
 ___builtin_annot :: ___builtin_sel :: ___builtin_memcpy_aligned ::
 ___builtin_sqrt :: ___builtin_fsqrt :: ___builtin_fabsf ::
 ___builtin_fabs :: ___builtin_ctzll :: ___builtin_ctzl :: ___builtin_ctz ::
 ___builtin_clzll :: ___builtin_clzl :: ___builtin_clz ::
 ___builtin_bswap16 :: ___builtin_bswap32 :: ___builtin_bswap ::
 ___builtin_bswap64 :: ___builtin_ais_annot :: ___compcert_i64_umulh ::
 ___compcert_i64_smulh :: ___compcert_i64_sar :: ___compcert_i64_shr ::
 ___compcert_i64_shl :: ___compcert_i64_umod :: ___compcert_i64_smod ::
 ___compcert_i64_udiv :: ___compcert_i64_sdiv :: ___compcert_i64_utof ::
 ___compcert_i64_stof :: ___compcert_i64_utod :: ___compcert_i64_stod ::
 ___compcert_i64_dtou :: ___compcert_i64_dtos :: ___compcert_va_composite ::
 ___compcert_va_float64 :: ___compcert_va_int64 :: ___compcert_va_int32 ::
 nil).

Definition prog : Clight.program := 
  mkprogram composites global_definitions public_idents _main Logic.I.


