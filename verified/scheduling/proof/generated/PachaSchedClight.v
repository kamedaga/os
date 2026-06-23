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
  Definition source_file := "/home/kamer/os/verified/scheduling/src/pacha_sched.c".
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
Definition _clear_cpu_current : ident := $"clear_cpu_current".
Definition _clear_current_if_matches : ident := $"clear_current_if_matches".
Definition _cpu : ident := $"cpu".
Definition _cpu_count : ident := $"cpu_count".
Definition _cpu_has_current : ident := $"cpu_has_current".
Definition _cpu_id : ident := $"cpu_id".
Definition _cpus : ident := $"cpus".
Definition _current_entity_index : ident := $"current_entity_index".
Definition _current_generation : ident := $"current_generation".
Definition _current_index : ident := $"current_index".
Definition _current_thread_id : ident := $"current_thread_id".
Definition _deadline : ident := $"deadline".
Definition _decision_out : ident := $"decision_out".
Definition _eligible_time : ident := $"eligible_time".
Definition _entities : ident := $"entities".
Definition _entity : ident := $"entity".
Definition _entity_count : ident := $"entity_count".
Definition _finish_current_valid : ident := $"finish_current_valid".
Definition _generation : ident := $"generation".
Definition _has_current : ident := $"has_current".
Definition _has_entity : ident := $"has_entity".
Definition _i : ident := $"i".
Definition _idle_decision : ident := $"idle_decision".
Definition _index : ident := $"index".
Definition _index_out : ident := $"index_out".
Definition _kind : ident := $"kind".
Definition _main : ident := $"main".
Definition _map_eevdf_rc : ident := $"map_eevdf_rc".
Definition _mark : ident := $"mark".
Definition _min_vruntime : ident := $"min_vruntime".
Definition _on_timer_valid : ident := $"on_timer_valid".
Definition _out : ident := $"out".
Definition _pacha_eevdf_add : ident := $"pacha_eevdf_add".
Definition _pacha_eevdf_block : ident := $"pacha_eevdf_block".
Definition _pacha_eevdf_charge : ident := $"pacha_eevdf_charge".
Definition _pacha_eevdf_copy_runqueue : ident := $"pacha_eevdf_copy_runqueue".
Definition _pacha_eevdf_empty_runqueue : ident := $"pacha_eevdf_empty_runqueue".
Definition _pacha_eevdf_entity : ident := $"pacha_eevdf_entity".
Definition _pacha_eevdf_exit : ident := $"pacha_eevdf_exit".
Definition _pacha_eevdf_mark_running : ident := $"pacha_eevdf_mark_running".
Definition _pacha_eevdf_pick : ident := $"pacha_eevdf_pick".
Definition _pacha_eevdf_pick_result : ident := $"pacha_eevdf_pick_result".
Definition _pacha_eevdf_requeue_running : ident := $"pacha_eevdf_requeue_running".
Definition _pacha_eevdf_runqueue : ident := $"pacha_eevdf_runqueue".
Definition _pacha_eevdf_wake : ident := $"pacha_eevdf_wake".
Definition _pacha_sched_add_thread : ident := $"pacha_sched_add_thread".
Definition _pacha_sched_block_thread : ident := $"pacha_sched_block_thread".
Definition _pacha_sched_cpu : ident := $"pacha_sched_cpu".
Definition _pacha_sched_decision : ident := $"pacha_sched_decision".
Definition _pacha_sched_empty_state : ident := $"pacha_sched_empty_state".
Definition _pacha_sched_exit_thread : ident := $"pacha_sched_exit_thread".
Definition _pacha_sched_finish_current : ident := $"pacha_sched_finish_current".
Definition _pacha_sched_no_decision : ident := $"pacha_sched_no_decision".
Definition _pacha_sched_on_timer : ident := $"pacha_sched_on_timer".
Definition _pacha_sched_pick : ident := $"pacha_sched_pick".
Definition _pacha_sched_state : ident := $"pacha_sched_state".
Definition _pacha_sched_wake_thread : ident := $"pacha_sched_wake_thread".
Definition _pick_rc : ident := $"pick_rc".
Definition _pick_scratch : ident := $"pick_scratch".
Definition _rc : ident := $"rc".
Definition _result : ident := $"result".
Definition _rq : ident := $"rq".
Definition _run_thread_decision : ident := $"run_thread_decision".
Definition _runnable_count : ident := $"runnable_count".
Definition _runqueue : ident := $"runqueue".
Definition _runtime_ns : ident := $"runtime_ns".
Definition _sched : ident := $"sched".
Definition _sched_cpu_const_ptr : ident := $"sched_cpu_const_ptr".
Definition _sched_cpu_has_current : ident := $"sched_cpu_has_current".
Definition _sched_cpu_ptr : ident := $"sched_cpu_ptr".
Definition _scratch : ident := $"scratch".
Definition _service_ns : ident := $"service_ns".
Definition _set_cpu_current : ident := $"set_cpu_current".
Definition _slice_ns : ident := $"slice_ns".
Definition _slot : ident := $"slot".
Definition _state : ident := $"state".
Definition _thread_id : ident := $"thread_id".
Definition _valid_cpu : ident := $"valid_cpu".
Definition _virtual_time : ident := $"virtual_time".
Definition _vruntime : ident := $"vruntime".
Definition _weight : ident := $"weight".
Definition _t'1 : ident := 128%positive.
Definition _t'10 : ident := 137%positive.
Definition _t'2 : ident := 129%positive.
Definition _t'3 : ident := 130%positive.
Definition _t'4 : ident := 131%positive.
Definition _t'5 : ident := 132%positive.
Definition _t'6 : ident := 133%positive.
Definition _t'7 : ident := 134%positive.
Definition _t'8 : ident := 135%positive.
Definition _t'9 : ident := 136%positive.

Definition f_map_eevdf_rc := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_rc, tint) :: nil);
  fn_vars := nil;
  fn_temps := nil;
  fn_body :=
(Ssequence
  (Sswitch (Etempvar _rc tint)
    (LScons (Some 0)
      (Sreturn (Some (Econst_int (Int.repr 0) tint)))
      (LScons (Some 1)
        (Sreturn (Some (Econst_int (Int.repr 1) tint)))
        (LScons (Some 2)
          (Sreturn (Some (Econst_int (Int.repr 2) tint)))
          (LScons (Some 3)
            (Sreturn (Some (Econst_int (Int.repr 3) tint)))
            (LScons (Some 4)
              (Sreturn (Some (Econst_int (Int.repr 4) tint)))
              LSnil))))))
  (Sreturn (Some (Econst_int (Int.repr 1) tint))))
|}.

Definition f_pacha_sched_no_decision := {|
  fn_return := tvoid;
  fn_callconv := cc_default;
  fn_params := ((_out, (tptr (Tstruct _pacha_sched_decision noattr))) :: nil);
  fn_vars := nil;
  fn_temps := nil;
  fn_body :=
(Ssequence
  (Sassign
    (Efield
      (Ederef (Etempvar _out (tptr (Tstruct _pacha_sched_decision noattr)))
        (Tstruct _pacha_sched_decision noattr)) _kind tint)
    (Econst_int (Int.repr 0) tint))
  (Ssequence
    (Sassign
      (Efield
        (Ederef (Etempvar _out (tptr (Tstruct _pacha_sched_decision noattr)))
          (Tstruct _pacha_sched_decision noattr)) _cpu_id tulong)
      (Econst_int (Int.repr 256) tuint))
    (Ssequence
      (Sassign
        (Efield
          (Ederef
            (Etempvar _out (tptr (Tstruct _pacha_sched_decision noattr)))
            (Tstruct _pacha_sched_decision noattr)) _thread_id tlong)
        (Econst_int (Int.repr 0) tint))
      (Ssequence
        (Sassign
          (Efield
            (Ederef
              (Etempvar _out (tptr (Tstruct _pacha_sched_decision noattr)))
              (Tstruct _pacha_sched_decision noattr)) _generation tlong)
          (Econst_int (Int.repr 0) tint))
        (Sreturn None)))))
|}.

Definition f_run_thread_decision := {|
  fn_return := tvoid;
  fn_callconv := cc_default;
  fn_params := ((_cpu_id, tulong) ::
                (_entity, (tptr (Tstruct _pacha_eevdf_entity noattr))) ::
                (_out, (tptr (Tstruct _pacha_sched_decision noattr))) :: nil);
  fn_vars := nil;
  fn_temps := ((_t'2, tlong) :: (_t'1, tlong) :: nil);
  fn_body :=
(Ssequence
  (Sassign
    (Efield
      (Ederef (Etempvar _out (tptr (Tstruct _pacha_sched_decision noattr)))
        (Tstruct _pacha_sched_decision noattr)) _kind tint)
    (Econst_int (Int.repr 1) tint))
  (Ssequence
    (Sassign
      (Efield
        (Ederef (Etempvar _out (tptr (Tstruct _pacha_sched_decision noattr)))
          (Tstruct _pacha_sched_decision noattr)) _cpu_id tulong)
      (Etempvar _cpu_id tulong))
    (Ssequence
      (Ssequence
        (Sset _t'2
          (Efield
            (Ederef
              (Etempvar _entity (tptr (Tstruct _pacha_eevdf_entity noattr)))
              (Tstruct _pacha_eevdf_entity noattr)) _thread_id tlong))
        (Sassign
          (Efield
            (Ederef
              (Etempvar _out (tptr (Tstruct _pacha_sched_decision noattr)))
              (Tstruct _pacha_sched_decision noattr)) _thread_id tlong)
          (Etempvar _t'2 tlong)))
      (Ssequence
        (Ssequence
          (Sset _t'1
            (Efield
              (Ederef
                (Etempvar _entity (tptr (Tstruct _pacha_eevdf_entity noattr)))
                (Tstruct _pacha_eevdf_entity noattr)) _generation tlong))
          (Sassign
            (Efield
              (Ederef
                (Etempvar _out (tptr (Tstruct _pacha_sched_decision noattr)))
                (Tstruct _pacha_sched_decision noattr)) _generation tlong)
            (Etempvar _t'1 tlong)))
        (Sreturn None)))))
|}.

Definition f_idle_decision := {|
  fn_return := tvoid;
  fn_callconv := cc_default;
  fn_params := ((_cpu_id, tulong) ::
                (_out, (tptr (Tstruct _pacha_sched_decision noattr))) :: nil);
  fn_vars := nil;
  fn_temps := nil;
  fn_body :=
(Ssequence
  (Sassign
    (Efield
      (Ederef (Etempvar _out (tptr (Tstruct _pacha_sched_decision noattr)))
        (Tstruct _pacha_sched_decision noattr)) _kind tint)
    (Econst_int (Int.repr 2) tint))
  (Ssequence
    (Sassign
      (Efield
        (Ederef (Etempvar _out (tptr (Tstruct _pacha_sched_decision noattr)))
          (Tstruct _pacha_sched_decision noattr)) _cpu_id tulong)
      (Etempvar _cpu_id tulong))
    (Ssequence
      (Sassign
        (Efield
          (Ederef
            (Etempvar _out (tptr (Tstruct _pacha_sched_decision noattr)))
            (Tstruct _pacha_sched_decision noattr)) _thread_id tlong)
        (Econst_int (Int.repr 0) tint))
      (Ssequence
        (Sassign
          (Efield
            (Ederef
              (Etempvar _out (tptr (Tstruct _pacha_sched_decision noattr)))
              (Tstruct _pacha_sched_decision noattr)) _generation tlong)
          (Econst_int (Int.repr 0) tint))
        (Sreturn None)))))
|}.

Definition f_valid_cpu := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_sched, (tptr (Tstruct _pacha_sched_state noattr))) ::
                (_cpu_id, tulong) :: nil);
  fn_vars := nil;
  fn_temps := ((_t'1, tulong) :: nil);
  fn_body :=
(Ssequence
  (Sset _t'1
    (Efield
      (Ederef (Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr)))
        (Tstruct _pacha_sched_state noattr)) _cpu_count tulong))
  (Sreturn (Some (Ebinop Olt (Etempvar _cpu_id tulong) (Etempvar _t'1 tulong)
                   tint))))
|}.

Definition f_sched_cpu_const_ptr := {|
  fn_return := (tptr (Tstruct _pacha_sched_cpu noattr));
  fn_callconv := cc_default;
  fn_params := ((_sched, (tptr (Tstruct _pacha_sched_state noattr))) ::
                (_cpu_id, tulong) :: nil);
  fn_vars := nil;
  fn_temps := nil;
  fn_body :=
(Sreturn (Some (Ebinop Oadd
                 (Efield
                   (Ederef
                     (Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr)))
                     (Tstruct _pacha_sched_state noattr)) _cpus
                   (tarray (Tstruct _pacha_sched_cpu noattr) 256))
                 (Etempvar _cpu_id tulong)
                 (tptr (Tstruct _pacha_sched_cpu noattr)))))
|}.

Definition f_sched_cpu_ptr := {|
  fn_return := (tptr (Tstruct _pacha_sched_cpu noattr));
  fn_callconv := cc_default;
  fn_params := ((_sched, (tptr (Tstruct _pacha_sched_state noattr))) ::
                (_cpu_id, tulong) :: nil);
  fn_vars := nil;
  fn_temps := nil;
  fn_body :=
(Sreturn (Some (Ebinop Oadd
                 (Efield
                   (Ederef
                     (Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr)))
                     (Tstruct _pacha_sched_state noattr)) _cpus
                   (tarray (Tstruct _pacha_sched_cpu noattr) 256))
                 (Etempvar _cpu_id tulong)
                 (tptr (Tstruct _pacha_sched_cpu noattr)))))
|}.

Definition f_sched_cpu_has_current := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_cpu, (tptr (Tstruct _pacha_sched_cpu noattr))) :: nil);
  fn_vars := nil;
  fn_temps := ((_t'1, tint) :: nil);
  fn_body :=
(Ssequence
  (Sset _t'1
    (Efield
      (Ederef (Etempvar _cpu (tptr (Tstruct _pacha_sched_cpu noattr)))
        (Tstruct _pacha_sched_cpu noattr)) _has_current tint))
  (Sreturn (Some (Etempvar _t'1 tint))))
|}.

Definition f_cpu_has_current := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_sched, (tptr (Tstruct _pacha_sched_state noattr))) ::
                (_cpu_id, tulong) :: nil);
  fn_vars := nil;
  fn_temps := ((_result, tint) :: (_t'3, tint) :: (_t'2, tint) ::
               (_t'1, (tptr (Tstruct _pacha_sched_cpu noattr))) :: nil);
  fn_body :=
(Ssequence
  (Sset _result (Econst_int (Int.repr 0) tint))
  (Ssequence
    (Ssequence
      (Scall (Some _t'3)
        (Evar _valid_cpu (Tfunction
                           ((tptr (Tstruct _pacha_sched_state noattr)) ::
                            tulong :: nil) tint cc_default))
        ((Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr))) ::
         (Etempvar _cpu_id tulong) :: nil))
      (Sifthenelse (Etempvar _t'3 tint)
        (Ssequence
          (Ssequence
            (Scall (Some _t'1)
              (Evar _sched_cpu_const_ptr (Tfunction
                                           ((tptr (Tstruct _pacha_sched_state noattr)) ::
                                            tulong :: nil)
                                           (tptr (Tstruct _pacha_sched_cpu noattr))
                                           cc_default))
              ((Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr))) ::
               (Etempvar _cpu_id tulong) :: nil))
            (Scall (Some _t'2)
              (Evar _sched_cpu_has_current (Tfunction
                                             ((tptr (Tstruct _pacha_sched_cpu noattr)) ::
                                              nil) tint cc_default))
              ((Etempvar _t'1 (tptr (Tstruct _pacha_sched_cpu noattr))) ::
               nil)))
          (Sset _result (Etempvar _t'2 tint)))
        Sskip))
    (Sreturn (Some (Etempvar _result tint)))))
|}.

Definition f_clear_cpu_current := {|
  fn_return := tvoid;
  fn_callconv := cc_default;
  fn_params := ((_cpu, (tptr (Tstruct _pacha_sched_cpu noattr))) :: nil);
  fn_vars := nil;
  fn_temps := nil;
  fn_body :=
(Ssequence
  (Sassign
    (Efield
      (Ederef (Etempvar _cpu (tptr (Tstruct _pacha_sched_cpu noattr)))
        (Tstruct _pacha_sched_cpu noattr)) _has_current tint)
    (Econst_int (Int.repr 0) tint))
  (Ssequence
    (Sassign
      (Efield
        (Ederef (Etempvar _cpu (tptr (Tstruct _pacha_sched_cpu noattr)))
          (Tstruct _pacha_sched_cpu noattr)) _current_thread_id tlong)
      (Econst_int (Int.repr 0) tint))
    (Ssequence
      (Sassign
        (Efield
          (Ederef (Etempvar _cpu (tptr (Tstruct _pacha_sched_cpu noattr)))
            (Tstruct _pacha_sched_cpu noattr)) _current_generation tlong)
        (Econst_int (Int.repr 0) tint))
      (Sreturn None))))
|}.

Definition f_set_cpu_current := {|
  fn_return := tvoid;
  fn_callconv := cc_default;
  fn_params := ((_cpu, (tptr (Tstruct _pacha_sched_cpu noattr))) ::
                (_thread_id, tlong) :: (_generation, tlong) :: nil);
  fn_vars := nil;
  fn_temps := nil;
  fn_body :=
(Ssequence
  (Sassign
    (Efield
      (Ederef (Etempvar _cpu (tptr (Tstruct _pacha_sched_cpu noattr)))
        (Tstruct _pacha_sched_cpu noattr)) _has_current tint)
    (Econst_int (Int.repr 1) tint))
  (Ssequence
    (Sassign
      (Efield
        (Ederef (Etempvar _cpu (tptr (Tstruct _pacha_sched_cpu noattr)))
          (Tstruct _pacha_sched_cpu noattr)) _current_thread_id tlong)
      (Etempvar _thread_id tlong))
    (Ssequence
      (Sassign
        (Efield
          (Ederef (Etempvar _cpu (tptr (Tstruct _pacha_sched_cpu noattr)))
            (Tstruct _pacha_sched_cpu noattr)) _current_generation tlong)
        (Etempvar _generation tlong))
      (Sreturn None))))
|}.

Definition f_clear_current_if_matches := {|
  fn_return := tvoid;
  fn_callconv := cc_default;
  fn_params := ((_sched, (tptr (Tstruct _pacha_sched_state noattr))) ::
                (_thread_id, tlong) :: nil);
  fn_vars := nil;
  fn_temps := ((_cpu, tulong) ::
               (_slot, (tptr (Tstruct _pacha_sched_cpu noattr))) ::
               (_t'2, tint) ::
               (_t'1, (tptr (Tstruct _pacha_sched_cpu noattr))) ::
               (_t'5, tulong) :: (_t'4, tlong) :: (_t'3, tint) :: nil);
  fn_body :=
(Ssequence
  (Sset _cpu (Ecast (Econst_int (Int.repr 0) tint) tulong))
  (Sloop
    (Ssequence
      (Ssequence
        (Sset _t'5
          (Efield
            (Ederef
              (Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr)))
              (Tstruct _pacha_sched_state noattr)) _cpu_count tulong))
        (Sifthenelse (Ebinop Olt (Etempvar _cpu tulong)
                       (Etempvar _t'5 tulong) tint)
          Sskip
          Sbreak))
      (Ssequence
        (Ssequence
          (Scall (Some _t'1)
            (Evar _sched_cpu_ptr (Tfunction
                                   ((tptr (Tstruct _pacha_sched_state noattr)) ::
                                    tulong :: nil)
                                   (tptr (Tstruct _pacha_sched_cpu noattr))
                                   cc_default))
            ((Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr))) ::
             (Etempvar _cpu tulong) :: nil))
          (Sset _slot
            (Etempvar _t'1 (tptr (Tstruct _pacha_sched_cpu noattr)))))
        (Ssequence
          (Ssequence
            (Sset _t'3
              (Efield
                (Ederef
                  (Etempvar _slot (tptr (Tstruct _pacha_sched_cpu noattr)))
                  (Tstruct _pacha_sched_cpu noattr)) _has_current tint))
            (Sifthenelse (Etempvar _t'3 tint)
              (Ssequence
                (Sset _t'4
                  (Efield
                    (Ederef
                      (Etempvar _slot (tptr (Tstruct _pacha_sched_cpu noattr)))
                      (Tstruct _pacha_sched_cpu noattr)) _current_thread_id
                    tlong))
                (Sset _t'2
                  (Ecast
                    (Ebinop Oeq (Etempvar _t'4 tlong)
                      (Etempvar _thread_id tlong) tint) tbool)))
              (Sset _t'2 (Econst_int (Int.repr 0) tint))))
          (Sifthenelse (Etempvar _t'2 tint)
            (Scall None
              (Evar _clear_cpu_current (Tfunction
                                         ((tptr (Tstruct _pacha_sched_cpu noattr)) ::
                                          nil) tvoid cc_default))
              ((Etempvar _slot (tptr (Tstruct _pacha_sched_cpu noattr))) ::
               nil))
            Sskip))))
    (Sset _cpu
      (Ebinop Oadd (Etempvar _cpu tulong) (Econst_int (Int.repr 1) tint)
        tulong))))
|}.

Definition f_current_entity_index := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_sched, (tptr (Tstruct _pacha_sched_state noattr))) ::
                (_cpu, (tptr (Tstruct _pacha_sched_cpu noattr))) ::
                (_index_out, (tptr tulong)) :: nil);
  fn_vars := nil;
  fn_temps := ((_i, tulong) ::
               (_entity, (tptr (Tstruct _pacha_eevdf_entity noattr))) ::
               (_t'3, tint) :: (_t'2, tint) :: (_t'1, tint) ::
               (_t'9, tulong) :: (_t'8, tlong) :: (_t'7, tlong) ::
               (_t'6, tlong) :: (_t'5, tlong) :: (_t'4, tint) :: nil);
  fn_body :=
(Ssequence
  (Ssequence
    (Scall (Some _t'1)
      (Evar _sched_cpu_has_current (Tfunction
                                     ((tptr (Tstruct _pacha_sched_cpu noattr)) ::
                                      nil) tint cc_default))
      ((Etempvar _cpu (tptr (Tstruct _pacha_sched_cpu noattr))) :: nil))
    (Sifthenelse (Eunop Onotbool (Etempvar _t'1 tint) tint)
      (Sreturn (Some (Econst_int (Int.repr 0) tint)))
      Sskip))
  (Ssequence
    (Ssequence
      (Sset _i (Ecast (Econst_int (Int.repr 0) tint) tulong))
      (Sloop
        (Ssequence
          (Ssequence
            (Sset _t'9
              (Efield
                (Efield
                  (Ederef
                    (Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr)))
                    (Tstruct _pacha_sched_state noattr)) _runqueue
                  (Tstruct _pacha_eevdf_runqueue noattr)) _entity_count
                tulong))
            (Sifthenelse (Ebinop Olt (Etempvar _i tulong)
                           (Etempvar _t'9 tulong) tint)
              Sskip
              Sbreak))
          (Ssequence
            (Sset _entity
              (Ebinop Oadd
                (Efield
                  (Efield
                    (Ederef
                      (Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr)))
                      (Tstruct _pacha_sched_state noattr)) _runqueue
                    (Tstruct _pacha_eevdf_runqueue noattr)) _entities
                  (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                (Etempvar _i tulong)
                (tptr (Tstruct _pacha_eevdf_entity noattr))))
            (Ssequence
              (Ssequence
                (Ssequence
                  (Sset _t'5
                    (Efield
                      (Ederef
                        (Etempvar _entity (tptr (Tstruct _pacha_eevdf_entity noattr)))
                        (Tstruct _pacha_eevdf_entity noattr)) _thread_id
                      tlong))
                  (Ssequence
                    (Sset _t'6
                      (Efield
                        (Ederef
                          (Etempvar _cpu (tptr (Tstruct _pacha_sched_cpu noattr)))
                          (Tstruct _pacha_sched_cpu noattr))
                        _current_thread_id tlong))
                    (Sifthenelse (Ebinop Oeq (Etempvar _t'5 tlong)
                                   (Etempvar _t'6 tlong) tint)
                      (Ssequence
                        (Sset _t'7
                          (Efield
                            (Ederef
                              (Etempvar _entity (tptr (Tstruct _pacha_eevdf_entity noattr)))
                              (Tstruct _pacha_eevdf_entity noattr))
                            _generation tlong))
                        (Ssequence
                          (Sset _t'8
                            (Efield
                              (Ederef
                                (Etempvar _cpu (tptr (Tstruct _pacha_sched_cpu noattr)))
                                (Tstruct _pacha_sched_cpu noattr))
                              _current_generation tlong))
                          (Sset _t'2
                            (Ecast
                              (Ebinop Oeq (Etempvar _t'7 tlong)
                                (Etempvar _t'8 tlong) tint) tbool))))
                      (Sset _t'2 (Econst_int (Int.repr 0) tint)))))
                (Sifthenelse (Etempvar _t'2 tint)
                  (Ssequence
                    (Sset _t'4
                      (Efield
                        (Ederef
                          (Etempvar _entity (tptr (Tstruct _pacha_eevdf_entity noattr)))
                          (Tstruct _pacha_eevdf_entity noattr)) _state tint))
                    (Sset _t'3
                      (Ecast
                        (Ebinop Oeq (Etempvar _t'4 tint)
                          (Econst_int (Int.repr 2) tint) tint) tbool)))
                  (Sset _t'3 (Econst_int (Int.repr 0) tint))))
              (Sifthenelse (Etempvar _t'3 tint)
                (Ssequence
                  (Sassign
                    (Ederef (Etempvar _index_out (tptr tulong)) tulong)
                    (Etempvar _i tulong))
                  (Sreturn (Some (Econst_int (Int.repr 1) tint))))
                Sskip))))
        (Sset _i
          (Ebinop Oadd (Etempvar _i tulong) (Econst_int (Int.repr 1) tint)
            tulong))))
    (Sreturn (Some (Econst_int (Int.repr 0) tint)))))
|}.

Definition f_pacha_sched_empty_state := {|
  fn_return := tvoid;
  fn_callconv := cc_default;
  fn_params := ((_cpu_count, tulong) ::
                (_out, (tptr (Tstruct _pacha_sched_state noattr))) :: nil);
  fn_vars := nil;
  fn_temps := ((_i, tulong) :: (_t'1, tulong) :: nil);
  fn_body :=
(Ssequence
  (Scall None
    (Evar _pacha_eevdf_empty_runqueue (Tfunction
                                        ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                         nil) tvoid cc_default))
    ((Eaddrof
       (Efield
         (Ederef (Etempvar _out (tptr (Tstruct _pacha_sched_state noattr)))
           (Tstruct _pacha_sched_state noattr)) _runqueue
         (Tstruct _pacha_eevdf_runqueue noattr))
       (tptr (Tstruct _pacha_eevdf_runqueue noattr))) :: nil))
  (Ssequence
    (Sassign
      (Efield
        (Ederef (Etempvar _out (tptr (Tstruct _pacha_sched_state noattr)))
          (Tstruct _pacha_sched_state noattr)) _cpu_count tulong)
      (Etempvar _cpu_count tulong))
    (Ssequence
      (Ssequence
        (Sset _t'1
          (Efield
            (Ederef
              (Etempvar _out (tptr (Tstruct _pacha_sched_state noattr)))
              (Tstruct _pacha_sched_state noattr)) _cpu_count tulong))
        (Sifthenelse (Ebinop Ogt (Etempvar _t'1 tulong)
                       (Econst_int (Int.repr 256) tuint) tint)
          (Sassign
            (Efield
              (Ederef
                (Etempvar _out (tptr (Tstruct _pacha_sched_state noattr)))
                (Tstruct _pacha_sched_state noattr)) _cpu_count tulong)
            (Econst_int (Int.repr 256) tuint))
          Sskip))
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
                (Sassign
                  (Efield
                    (Ederef
                      (Ebinop Oadd
                        (Efield
                          (Ederef
                            (Etempvar _out (tptr (Tstruct _pacha_sched_state noattr)))
                            (Tstruct _pacha_sched_state noattr)) _cpus
                          (tarray (Tstruct _pacha_sched_cpu noattr) 256))
                        (Etempvar _i tulong)
                        (tptr (Tstruct _pacha_sched_cpu noattr)))
                      (Tstruct _pacha_sched_cpu noattr)) _has_current tint)
                  (Econst_int (Int.repr 0) tint))
                (Ssequence
                  (Sassign
                    (Efield
                      (Ederef
                        (Ebinop Oadd
                          (Efield
                            (Ederef
                              (Etempvar _out (tptr (Tstruct _pacha_sched_state noattr)))
                              (Tstruct _pacha_sched_state noattr)) _cpus
                            (tarray (Tstruct _pacha_sched_cpu noattr) 256))
                          (Etempvar _i tulong)
                          (tptr (Tstruct _pacha_sched_cpu noattr)))
                        (Tstruct _pacha_sched_cpu noattr)) _current_thread_id
                      tlong) (Econst_int (Int.repr 0) tint))
                  (Sassign
                    (Efield
                      (Ederef
                        (Ebinop Oadd
                          (Efield
                            (Ederef
                              (Etempvar _out (tptr (Tstruct _pacha_sched_state noattr)))
                              (Tstruct _pacha_sched_state noattr)) _cpus
                            (tarray (Tstruct _pacha_sched_cpu noattr) 256))
                          (Etempvar _i tulong)
                          (tptr (Tstruct _pacha_sched_cpu noattr)))
                        (Tstruct _pacha_sched_cpu noattr))
                      _current_generation tlong)
                    (Econst_int (Int.repr 0) tint)))))
            (Sset _i
              (Ebinop Oadd (Etempvar _i tulong)
                (Econst_int (Int.repr 1) tint) tulong))))
        (Sreturn None)))))
|}.

Definition f_pacha_sched_add_thread := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_sched, (tptr (Tstruct _pacha_sched_state noattr))) ::
                (_thread_id, tlong) :: (_generation, tlong) ::
                (_weight, tlong) :: (_slice_ns, tlong) ::
                (_decision_out,
                 (tptr (Tstruct _pacha_sched_decision noattr))) ::
                (_scratch, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                nil);
  fn_vars := nil;
  fn_temps := ((_rc, tint) :: (_t'2, tint) :: (_t'1, tint) :: nil);
  fn_body :=
(Ssequence
  (Scall None
    (Evar _pacha_sched_no_decision (Tfunction
                                     ((tptr (Tstruct _pacha_sched_decision noattr)) ::
                                      nil) tvoid cc_default))
    ((Etempvar _decision_out (tptr (Tstruct _pacha_sched_decision noattr))) ::
     nil))
  (Ssequence
    (Ssequence
      (Scall (Some _t'1)
        (Evar _pacha_eevdf_add (Tfunction
                                 ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                  tlong :: tlong :: tlong :: tlong ::
                                  (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                  nil) tint cc_default))
        ((Eaddrof
           (Efield
             (Ederef
               (Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr)))
               (Tstruct _pacha_sched_state noattr)) _runqueue
             (Tstruct _pacha_eevdf_runqueue noattr))
           (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
         (Etempvar _thread_id tlong) :: (Etempvar _generation tlong) ::
         (Etempvar _weight tlong) :: (Etempvar _slice_ns tlong) ::
         (Etempvar _scratch (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
         nil))
      (Sset _rc (Etempvar _t'1 tint)))
    (Ssequence
      (Sifthenelse (Ebinop One (Etempvar _rc tint)
                     (Econst_int (Int.repr 0) tint) tint)
        (Ssequence
          (Scall (Some _t'2)
            (Evar _map_eevdf_rc (Tfunction (tint :: nil) tint cc_default))
            ((Etempvar _rc tint) :: nil))
          (Sreturn (Some (Etempvar _t'2 tint))))
        Sskip)
      (Ssequence
        (Scall None
          (Evar _pacha_eevdf_copy_runqueue (Tfunction
                                             ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                              (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                              nil) tvoid cc_default))
          ((Etempvar _scratch (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
           (Eaddrof
             (Efield
               (Ederef
                 (Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr)))
                 (Tstruct _pacha_sched_state noattr)) _runqueue
               (Tstruct _pacha_eevdf_runqueue noattr))
             (tptr (Tstruct _pacha_eevdf_runqueue noattr))) :: nil))
        (Sreturn (Some (Econst_int (Int.repr 0) tint)))))))
|}.

Definition f_pacha_sched_wake_thread := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_sched, (tptr (Tstruct _pacha_sched_state noattr))) ::
                (_thread_id, tlong) ::
                (_decision_out,
                 (tptr (Tstruct _pacha_sched_decision noattr))) ::
                (_scratch, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                nil);
  fn_vars := nil;
  fn_temps := ((_rc, tint) :: (_t'2, tint) :: (_t'1, tint) :: nil);
  fn_body :=
(Ssequence
  (Scall None
    (Evar _pacha_sched_no_decision (Tfunction
                                     ((tptr (Tstruct _pacha_sched_decision noattr)) ::
                                      nil) tvoid cc_default))
    ((Etempvar _decision_out (tptr (Tstruct _pacha_sched_decision noattr))) ::
     nil))
  (Ssequence
    (Ssequence
      (Scall (Some _t'1)
        (Evar _pacha_eevdf_wake (Tfunction
                                  ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                   tlong ::
                                   (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                   nil) tint cc_default))
        ((Eaddrof
           (Efield
             (Ederef
               (Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr)))
               (Tstruct _pacha_sched_state noattr)) _runqueue
             (Tstruct _pacha_eevdf_runqueue noattr))
           (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
         (Etempvar _thread_id tlong) ::
         (Etempvar _scratch (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
         nil))
      (Sset _rc (Etempvar _t'1 tint)))
    (Ssequence
      (Sifthenelse (Ebinop One (Etempvar _rc tint)
                     (Econst_int (Int.repr 0) tint) tint)
        (Ssequence
          (Scall (Some _t'2)
            (Evar _map_eevdf_rc (Tfunction (tint :: nil) tint cc_default))
            ((Etempvar _rc tint) :: nil))
          (Sreturn (Some (Etempvar _t'2 tint))))
        Sskip)
      (Ssequence
        (Scall None
          (Evar _pacha_eevdf_copy_runqueue (Tfunction
                                             ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                              (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                              nil) tvoid cc_default))
          ((Etempvar _scratch (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
           (Eaddrof
             (Efield
               (Ederef
                 (Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr)))
                 (Tstruct _pacha_sched_state noattr)) _runqueue
               (Tstruct _pacha_eevdf_runqueue noattr))
             (tptr (Tstruct _pacha_eevdf_runqueue noattr))) :: nil))
        (Sreturn (Some (Econst_int (Int.repr 0) tint)))))))
|}.

Definition f_pacha_sched_block_thread := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_sched, (tptr (Tstruct _pacha_sched_state noattr))) ::
                (_thread_id, tlong) ::
                (_decision_out,
                 (tptr (Tstruct _pacha_sched_decision noattr))) ::
                (_scratch, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                nil);
  fn_vars := nil;
  fn_temps := ((_rc, tint) :: (_t'2, tint) :: (_t'1, tint) :: nil);
  fn_body :=
(Ssequence
  (Scall None
    (Evar _pacha_sched_no_decision (Tfunction
                                     ((tptr (Tstruct _pacha_sched_decision noattr)) ::
                                      nil) tvoid cc_default))
    ((Etempvar _decision_out (tptr (Tstruct _pacha_sched_decision noattr))) ::
     nil))
  (Ssequence
    (Ssequence
      (Scall (Some _t'1)
        (Evar _pacha_eevdf_block (Tfunction
                                   ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                    tlong ::
                                    (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                    nil) tint cc_default))
        ((Eaddrof
           (Efield
             (Ederef
               (Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr)))
               (Tstruct _pacha_sched_state noattr)) _runqueue
             (Tstruct _pacha_eevdf_runqueue noattr))
           (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
         (Etempvar _thread_id tlong) ::
         (Etempvar _scratch (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
         nil))
      (Sset _rc (Etempvar _t'1 tint)))
    (Ssequence
      (Sifthenelse (Ebinop One (Etempvar _rc tint)
                     (Econst_int (Int.repr 0) tint) tint)
        (Ssequence
          (Scall (Some _t'2)
            (Evar _map_eevdf_rc (Tfunction (tint :: nil) tint cc_default))
            ((Etempvar _rc tint) :: nil))
          (Sreturn (Some (Etempvar _t'2 tint))))
        Sskip)
      (Ssequence
        (Scall None
          (Evar _pacha_eevdf_copy_runqueue (Tfunction
                                             ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                              (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                              nil) tvoid cc_default))
          ((Etempvar _scratch (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
           (Eaddrof
             (Efield
               (Ederef
                 (Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr)))
                 (Tstruct _pacha_sched_state noattr)) _runqueue
               (Tstruct _pacha_eevdf_runqueue noattr))
             (tptr (Tstruct _pacha_eevdf_runqueue noattr))) :: nil))
        (Ssequence
          (Scall None
            (Evar _clear_current_if_matches (Tfunction
                                              ((tptr (Tstruct _pacha_sched_state noattr)) ::
                                               tlong :: nil) tvoid
                                              cc_default))
            ((Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr))) ::
             (Etempvar _thread_id tlong) :: nil))
          (Sreturn (Some (Econst_int (Int.repr 0) tint))))))))
|}.

Definition f_pacha_sched_exit_thread := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_sched, (tptr (Tstruct _pacha_sched_state noattr))) ::
                (_thread_id, tlong) ::
                (_decision_out,
                 (tptr (Tstruct _pacha_sched_decision noattr))) ::
                (_scratch, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                nil);
  fn_vars := nil;
  fn_temps := ((_rc, tint) :: (_t'2, tint) :: (_t'1, tint) :: nil);
  fn_body :=
(Ssequence
  (Scall None
    (Evar _pacha_sched_no_decision (Tfunction
                                     ((tptr (Tstruct _pacha_sched_decision noattr)) ::
                                      nil) tvoid cc_default))
    ((Etempvar _decision_out (tptr (Tstruct _pacha_sched_decision noattr))) ::
     nil))
  (Ssequence
    (Ssequence
      (Scall (Some _t'1)
        (Evar _pacha_eevdf_exit (Tfunction
                                  ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                   tlong ::
                                   (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                   nil) tint cc_default))
        ((Eaddrof
           (Efield
             (Ederef
               (Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr)))
               (Tstruct _pacha_sched_state noattr)) _runqueue
             (Tstruct _pacha_eevdf_runqueue noattr))
           (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
         (Etempvar _thread_id tlong) ::
         (Etempvar _scratch (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
         nil))
      (Sset _rc (Etempvar _t'1 tint)))
    (Ssequence
      (Sifthenelse (Ebinop One (Etempvar _rc tint)
                     (Econst_int (Int.repr 0) tint) tint)
        (Ssequence
          (Scall (Some _t'2)
            (Evar _map_eevdf_rc (Tfunction (tint :: nil) tint cc_default))
            ((Etempvar _rc tint) :: nil))
          (Sreturn (Some (Etempvar _t'2 tint))))
        Sskip)
      (Ssequence
        (Scall None
          (Evar _pacha_eevdf_copy_runqueue (Tfunction
                                             ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                              (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                              nil) tvoid cc_default))
          ((Etempvar _scratch (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
           (Eaddrof
             (Efield
               (Ederef
                 (Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr)))
                 (Tstruct _pacha_sched_state noattr)) _runqueue
               (Tstruct _pacha_eevdf_runqueue noattr))
             (tptr (Tstruct _pacha_eevdf_runqueue noattr))) :: nil))
        (Ssequence
          (Scall None
            (Evar _clear_current_if_matches (Tfunction
                                              ((tptr (Tstruct _pacha_sched_state noattr)) ::
                                               tlong :: nil) tvoid
                                              cc_default))
            ((Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr))) ::
             (Etempvar _thread_id tlong) :: nil))
          (Sreturn (Some (Econst_int (Int.repr 0) tint))))))))
|}.

Definition f_on_timer_valid := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_sched, (tptr (Tstruct _pacha_sched_state noattr))) ::
                (_cpu_id, tulong) :: (_runtime_ns, tlong) ::
                (_scratch, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                nil);
  fn_vars := ((_current_index, tulong) :: nil);
  fn_temps := ((_cpu, (tptr (Tstruct _pacha_sched_cpu noattr))) ::
               (_thread_id, tlong) :: (_rc, tint) :: (_t'5, tint) ::
               (_t'4, tint) :: (_t'3, tint) :: (_t'2, tint) ::
               (_t'1, (tptr (Tstruct _pacha_sched_cpu noattr))) ::
               (_t'6, tulong) :: nil);
  fn_body :=
(Ssequence
  (Ssequence
    (Scall (Some _t'1)
      (Evar _sched_cpu_ptr (Tfunction
                             ((tptr (Tstruct _pacha_sched_state noattr)) ::
                              tulong :: nil)
                             (tptr (Tstruct _pacha_sched_cpu noattr))
                             cc_default))
      ((Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr))) ::
       (Etempvar _cpu_id tulong) :: nil))
    (Sset _cpu (Etempvar _t'1 (tptr (Tstruct _pacha_sched_cpu noattr)))))
  (Ssequence
    (Ssequence
      (Scall (Some _t'5)
        (Evar _sched_cpu_has_current (Tfunction
                                       ((tptr (Tstruct _pacha_sched_cpu noattr)) ::
                                        nil) tint cc_default))
        ((Etempvar _cpu (tptr (Tstruct _pacha_sched_cpu noattr))) :: nil))
      (Sifthenelse (Etempvar _t'5 tint)
        (Ssequence
          (Sassign (Evar _current_index tulong)
            (Econst_int (Int.repr 0) tint))
          (Ssequence
            (Ssequence
              (Scall (Some _t'2)
                (Evar _current_entity_index (Tfunction
                                              ((tptr (Tstruct _pacha_sched_state noattr)) ::
                                               (tptr (Tstruct _pacha_sched_cpu noattr)) ::
                                               (tptr tulong) :: nil) tint
                                              cc_default))
                ((Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr))) ::
                 (Etempvar _cpu (tptr (Tstruct _pacha_sched_cpu noattr))) ::
                 (Eaddrof (Evar _current_index tulong) (tptr tulong)) :: nil))
              (Sifthenelse (Eunop Onotbool (Etempvar _t'2 tint) tint)
                (Ssequence
                  (Scall None
                    (Evar _clear_cpu_current (Tfunction
                                               ((tptr (Tstruct _pacha_sched_cpu noattr)) ::
                                                nil) tvoid cc_default))
                    ((Etempvar _cpu (tptr (Tstruct _pacha_sched_cpu noattr))) ::
                     nil))
                  (Sreturn (Some (Econst_int (Int.repr 0) tint))))
                Sskip))
            (Ssequence
              (Ssequence
                (Sset _t'6 (Evar _current_index tulong))
                (Sset _thread_id
                  (Efield
                    (Ederef
                      (Ebinop Oadd
                        (Efield
                          (Efield
                            (Ederef
                              (Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr)))
                              (Tstruct _pacha_sched_state noattr)) _runqueue
                            (Tstruct _pacha_eevdf_runqueue noattr)) _entities
                          (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                        (Etempvar _t'6 tulong)
                        (tptr (Tstruct _pacha_eevdf_entity noattr)))
                      (Tstruct _pacha_eevdf_entity noattr)) _thread_id tlong)))
              (Ssequence
                (Ssequence
                  (Scall (Some _t'3)
                    (Evar _pacha_eevdf_charge (Tfunction
                                                ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                                 tlong :: tlong ::
                                                 (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                                 nil) tint cc_default))
                    ((Eaddrof
                       (Efield
                         (Ederef
                           (Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr)))
                           (Tstruct _pacha_sched_state noattr)) _runqueue
                         (Tstruct _pacha_eevdf_runqueue noattr))
                       (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                     (Etempvar _thread_id tlong) ::
                     (Etempvar _runtime_ns tlong) ::
                     (Etempvar _scratch (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                     nil))
                  (Sset _rc (Etempvar _t'3 tint)))
                (Ssequence
                  (Sifthenelse (Ebinop One (Etempvar _rc tint)
                                 (Econst_int (Int.repr 0) tint) tint)
                    (Ssequence
                      (Scall (Some _t'4)
                        (Evar _map_eevdf_rc (Tfunction (tint :: nil) tint
                                              cc_default))
                        ((Etempvar _rc tint) :: nil))
                      (Sreturn (Some (Etempvar _t'4 tint))))
                    Sskip)
                  (Ssequence
                    (Scall None
                      (Evar _pacha_eevdf_copy_runqueue (Tfunction
                                                         ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                                          (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                                          nil) tvoid
                                                         cc_default))
                      ((Etempvar _scratch (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                       (Eaddrof
                         (Efield
                           (Ederef
                             (Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr)))
                             (Tstruct _pacha_sched_state noattr)) _runqueue
                           (Tstruct _pacha_eevdf_runqueue noattr))
                         (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                       nil))
                    (Sreturn (Some (Econst_int (Int.repr 0) tint)))))))))
        Sskip))
    (Sreturn (Some (Econst_int (Int.repr 0) tint)))))
|}.

Definition f_pacha_sched_on_timer := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_sched, (tptr (Tstruct _pacha_sched_state noattr))) ::
                (_cpu_id, tulong) :: (_runtime_ns, tlong) ::
                (_decision_out,
                 (tptr (Tstruct _pacha_sched_decision noattr))) ::
                (_scratch, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                nil);
  fn_vars := nil;
  fn_temps := ((_t'1, tint) :: (_t'2, tulong) :: nil);
  fn_body :=
(Ssequence
  (Scall None
    (Evar _pacha_sched_no_decision (Tfunction
                                     ((tptr (Tstruct _pacha_sched_decision noattr)) ::
                                      nil) tvoid cc_default))
    ((Etempvar _decision_out (tptr (Tstruct _pacha_sched_decision noattr))) ::
     nil))
  (Ssequence
    (Ssequence
      (Sset _t'2
        (Efield
          (Ederef
            (Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr)))
            (Tstruct _pacha_sched_state noattr)) _cpu_count tulong))
      (Sifthenelse (Ebinop Olt (Etempvar _cpu_id tulong)
                     (Etempvar _t'2 tulong) tint)
        (Ssequence
          (Scall (Some _t'1)
            (Evar _on_timer_valid (Tfunction
                                    ((tptr (Tstruct _pacha_sched_state noattr)) ::
                                     tulong :: tlong ::
                                     (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                     nil) tint cc_default))
            ((Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr))) ::
             (Etempvar _cpu_id tulong) :: (Etempvar _runtime_ns tlong) ::
             (Etempvar _scratch (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
             nil))
          (Sreturn (Some (Etempvar _t'1 tint))))
        Sskip))
    (Sreturn (Some (Econst_int (Int.repr 1) tint)))))
|}.

Definition f_pacha_sched_pick := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_sched, (tptr (Tstruct _pacha_sched_state noattr))) ::
                (_cpu_id, tulong) ::
                (_decision_out,
                 (tptr (Tstruct _pacha_sched_decision noattr))) ::
                (_pick_scratch,
                 (tptr (Tstruct _pacha_eevdf_pick_result noattr))) ::
                (_scratch, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                nil);
  fn_vars := nil;
  fn_temps := ((_pick_rc, tint) :: (_mark, tint) :: (_t'6, tint) ::
               (_t'5, tint) :: (_t'4, tint) :: (_t'3, tint) ::
               (_t'2, tint) :: (_t'1, tint) :: (_t'10, tint) ::
               (_t'9, tlong) :: (_t'8, tlong) :: (_t'7, tlong) :: nil);
  fn_body :=
(Ssequence
  (Scall None
    (Evar _pacha_sched_no_decision (Tfunction
                                     ((tptr (Tstruct _pacha_sched_decision noattr)) ::
                                      nil) tvoid cc_default))
    ((Etempvar _decision_out (tptr (Tstruct _pacha_sched_decision noattr))) ::
     nil))
  (Ssequence
    (Ssequence
      (Scall (Some _t'1)
        (Evar _valid_cpu (Tfunction
                           ((tptr (Tstruct _pacha_sched_state noattr)) ::
                            tulong :: nil) tint cc_default))
        ((Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr))) ::
         (Etempvar _cpu_id tulong) :: nil))
      (Sifthenelse (Eunop Onotbool (Etempvar _t'1 tint) tint)
        (Sreturn (Some (Econst_int (Int.repr 1) tint)))
        Sskip))
    (Ssequence
      (Ssequence
        (Scall (Some _t'2)
          (Evar _cpu_has_current (Tfunction
                                   ((tptr (Tstruct _pacha_sched_state noattr)) ::
                                    tulong :: nil) tint cc_default))
          ((Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr))) ::
           (Etempvar _cpu_id tulong) :: nil))
        (Sifthenelse (Etempvar _t'2 tint)
          (Sreturn (Some (Econst_int (Int.repr 4) tint)))
          Sskip))
      (Ssequence
        (Ssequence
          (Scall (Some _t'3)
            (Evar _pacha_eevdf_pick (Tfunction
                                      ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                       (tptr (Tstruct _pacha_eevdf_pick_result noattr)) ::
                                       nil) tint cc_default))
            ((Eaddrof
               (Efield
                 (Ederef
                   (Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr)))
                   (Tstruct _pacha_sched_state noattr)) _runqueue
                 (Tstruct _pacha_eevdf_runqueue noattr))
               (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
             (Etempvar _pick_scratch (tptr (Tstruct _pacha_eevdf_pick_result noattr))) ::
             nil))
          (Sset _pick_rc (Etempvar _t'3 tint)))
        (Ssequence
          (Sifthenelse (Ebinop One (Etempvar _pick_rc tint)
                         (Econst_int (Int.repr 0) tint) tint)
            (Ssequence
              (Scall (Some _t'4)
                (Evar _map_eevdf_rc (Tfunction (tint :: nil) tint cc_default))
                ((Etempvar _pick_rc tint) :: nil))
              (Sreturn (Some (Etempvar _t'4 tint))))
            Sskip)
          (Ssequence
            (Scall None
              (Evar _pacha_eevdf_copy_runqueue (Tfunction
                                                 ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                                  (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                                  nil) tvoid cc_default))
              ((Eaddrof
                 (Efield
                   (Ederef
                     (Etempvar _pick_scratch (tptr (Tstruct _pacha_eevdf_pick_result noattr)))
                     (Tstruct _pacha_eevdf_pick_result noattr)) _rq
                   (Tstruct _pacha_eevdf_runqueue noattr))
                 (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
               (Eaddrof
                 (Efield
                   (Ederef
                     (Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr)))
                     (Tstruct _pacha_sched_state noattr)) _runqueue
                   (Tstruct _pacha_eevdf_runqueue noattr))
                 (tptr (Tstruct _pacha_eevdf_runqueue noattr))) :: nil))
            (Ssequence
              (Ssequence
                (Sset _t'10
                  (Efield
                    (Ederef
                      (Etempvar _pick_scratch (tptr (Tstruct _pacha_eevdf_pick_result noattr)))
                      (Tstruct _pacha_eevdf_pick_result noattr)) _has_entity
                    tint))
                (Sifthenelse (Eunop Onotbool (Etempvar _t'10 tint) tint)
                  (Ssequence
                    (Scall None
                      (Evar _idle_decision (Tfunction
                                             (tulong ::
                                              (tptr (Tstruct _pacha_sched_decision noattr)) ::
                                              nil) tvoid cc_default))
                      ((Etempvar _cpu_id tulong) ::
                       (Etempvar _decision_out (tptr (Tstruct _pacha_sched_decision noattr))) ::
                       nil))
                    (Sreturn (Some (Econst_int (Int.repr 0) tint))))
                  Sskip))
              (Ssequence
                (Ssequence
                  (Ssequence
                    (Sset _t'9
                      (Efield
                        (Efield
                          (Ederef
                            (Etempvar _pick_scratch (tptr (Tstruct _pacha_eevdf_pick_result noattr)))
                            (Tstruct _pacha_eevdf_pick_result noattr))
                          _entity (Tstruct _pacha_eevdf_entity noattr))
                        _thread_id tlong))
                    (Scall (Some _t'5)
                      (Evar _pacha_eevdf_mark_running (Tfunction
                                                        ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                                         tlong ::
                                                         (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                                         nil) tint
                                                        cc_default))
                      ((Eaddrof
                         (Efield
                           (Ederef
                             (Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr)))
                             (Tstruct _pacha_sched_state noattr)) _runqueue
                           (Tstruct _pacha_eevdf_runqueue noattr))
                         (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                       (Etempvar _t'9 tlong) ::
                       (Etempvar _scratch (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                       nil)))
                  (Sset _mark (Etempvar _t'5 tint)))
                (Ssequence
                  (Sifthenelse (Ebinop One (Etempvar _mark tint)
                                 (Econst_int (Int.repr 0) tint) tint)
                    (Ssequence
                      (Scall None
                        (Evar _pacha_sched_no_decision (Tfunction
                                                         ((tptr (Tstruct _pacha_sched_decision noattr)) ::
                                                          nil) tvoid
                                                         cc_default))
                        ((Etempvar _decision_out (tptr (Tstruct _pacha_sched_decision noattr))) ::
                         nil))
                      (Ssequence
                        (Scall (Some _t'6)
                          (Evar _map_eevdf_rc (Tfunction (tint :: nil) tint
                                                cc_default))
                          ((Etempvar _mark tint) :: nil))
                        (Sreturn (Some (Etempvar _t'6 tint)))))
                    Sskip)
                  (Ssequence
                    (Scall None
                      (Evar _pacha_eevdf_copy_runqueue (Tfunction
                                                         ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                                          (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                                          nil) tvoid
                                                         cc_default))
                      ((Etempvar _scratch (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                       (Eaddrof
                         (Efield
                           (Ederef
                             (Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr)))
                             (Tstruct _pacha_sched_state noattr)) _runqueue
                           (Tstruct _pacha_eevdf_runqueue noattr))
                         (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                       nil))
                    (Ssequence
                      (Ssequence
                        (Sset _t'7
                          (Efield
                            (Efield
                              (Ederef
                                (Etempvar _pick_scratch (tptr (Tstruct _pacha_eevdf_pick_result noattr)))
                                (Tstruct _pacha_eevdf_pick_result noattr))
                              _entity (Tstruct _pacha_eevdf_entity noattr))
                            _thread_id tlong))
                        (Ssequence
                          (Sset _t'8
                            (Efield
                              (Efield
                                (Ederef
                                  (Etempvar _pick_scratch (tptr (Tstruct _pacha_eevdf_pick_result noattr)))
                                  (Tstruct _pacha_eevdf_pick_result noattr))
                                _entity (Tstruct _pacha_eevdf_entity noattr))
                              _generation tlong))
                          (Scall None
                            (Evar _set_cpu_current (Tfunction
                                                     ((tptr (Tstruct _pacha_sched_cpu noattr)) ::
                                                      tlong :: tlong :: nil)
                                                     tvoid cc_default))
                            ((Ebinop Oadd
                               (Efield
                                 (Ederef
                                   (Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr)))
                                   (Tstruct _pacha_sched_state noattr)) _cpus
                                 (tarray (Tstruct _pacha_sched_cpu noattr) 256))
                               (Etempvar _cpu_id tulong)
                               (tptr (Tstruct _pacha_sched_cpu noattr))) ::
                             (Etempvar _t'7 tlong) ::
                             (Etempvar _t'8 tlong) :: nil))))
                      (Ssequence
                        (Scall None
                          (Evar _run_thread_decision (Tfunction
                                                       (tulong ::
                                                        (tptr (Tstruct _pacha_eevdf_entity noattr)) ::
                                                        (tptr (Tstruct _pacha_sched_decision noattr)) ::
                                                        nil) tvoid
                                                       cc_default))
                          ((Etempvar _cpu_id tulong) ::
                           (Eaddrof
                             (Efield
                               (Ederef
                                 (Etempvar _pick_scratch (tptr (Tstruct _pacha_eevdf_pick_result noattr)))
                                 (Tstruct _pacha_eevdf_pick_result noattr))
                               _entity (Tstruct _pacha_eevdf_entity noattr))
                             (tptr (Tstruct _pacha_eevdf_entity noattr))) ::
                           (Etempvar _decision_out (tptr (Tstruct _pacha_sched_decision noattr))) ::
                           nil))
                        (Sreturn (Some (Econst_int (Int.repr 0) tint)))))))))))))))
|}.

Definition f_finish_current_valid := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_sched, (tptr (Tstruct _pacha_sched_state noattr))) ::
                (_cpu_id, tulong) ::
                (_scratch, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                nil);
  fn_vars := ((_current_index, tulong) :: nil);
  fn_temps := ((_cpu, (tptr (Tstruct _pacha_sched_cpu noattr))) ::
               (_thread_id, tlong) :: (_rc, tint) :: (_t'5, tint) ::
               (_t'4, tint) :: (_t'3, tint) :: (_t'2, tint) ::
               (_t'1, (tptr (Tstruct _pacha_sched_cpu noattr))) ::
               (_t'6, tulong) :: nil);
  fn_body :=
(Ssequence
  (Ssequence
    (Scall (Some _t'1)
      (Evar _sched_cpu_ptr (Tfunction
                             ((tptr (Tstruct _pacha_sched_state noattr)) ::
                              tulong :: nil)
                             (tptr (Tstruct _pacha_sched_cpu noattr))
                             cc_default))
      ((Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr))) ::
       (Etempvar _cpu_id tulong) :: nil))
    (Sset _cpu (Etempvar _t'1 (tptr (Tstruct _pacha_sched_cpu noattr)))))
  (Ssequence
    (Ssequence
      (Scall (Some _t'5)
        (Evar _sched_cpu_has_current (Tfunction
                                       ((tptr (Tstruct _pacha_sched_cpu noattr)) ::
                                        nil) tint cc_default))
        ((Etempvar _cpu (tptr (Tstruct _pacha_sched_cpu noattr))) :: nil))
      (Sifthenelse (Etempvar _t'5 tint)
        (Ssequence
          (Sassign (Evar _current_index tulong)
            (Econst_int (Int.repr 0) tint))
          (Ssequence
            (Ssequence
              (Scall (Some _t'2)
                (Evar _current_entity_index (Tfunction
                                              ((tptr (Tstruct _pacha_sched_state noattr)) ::
                                               (tptr (Tstruct _pacha_sched_cpu noattr)) ::
                                               (tptr tulong) :: nil) tint
                                              cc_default))
                ((Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr))) ::
                 (Etempvar _cpu (tptr (Tstruct _pacha_sched_cpu noattr))) ::
                 (Eaddrof (Evar _current_index tulong) (tptr tulong)) :: nil))
              (Sifthenelse (Eunop Onotbool (Etempvar _t'2 tint) tint)
                (Ssequence
                  (Scall None
                    (Evar _clear_cpu_current (Tfunction
                                               ((tptr (Tstruct _pacha_sched_cpu noattr)) ::
                                                nil) tvoid cc_default))
                    ((Etempvar _cpu (tptr (Tstruct _pacha_sched_cpu noattr))) ::
                     nil))
                  (Sreturn (Some (Econst_int (Int.repr 0) tint))))
                Sskip))
            (Ssequence
              (Ssequence
                (Sset _t'6 (Evar _current_index tulong))
                (Sset _thread_id
                  (Efield
                    (Ederef
                      (Ebinop Oadd
                        (Efield
                          (Efield
                            (Ederef
                              (Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr)))
                              (Tstruct _pacha_sched_state noattr)) _runqueue
                            (Tstruct _pacha_eevdf_runqueue noattr)) _entities
                          (tarray (Tstruct _pacha_eevdf_entity noattr) 256))
                        (Etempvar _t'6 tulong)
                        (tptr (Tstruct _pacha_eevdf_entity noattr)))
                      (Tstruct _pacha_eevdf_entity noattr)) _thread_id tlong)))
              (Ssequence
                (Ssequence
                  (Scall (Some _t'3)
                    (Evar _pacha_eevdf_requeue_running (Tfunction
                                                         ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                                          tlong ::
                                                          (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                                          nil) tint
                                                         cc_default))
                    ((Eaddrof
                       (Efield
                         (Ederef
                           (Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr)))
                           (Tstruct _pacha_sched_state noattr)) _runqueue
                         (Tstruct _pacha_eevdf_runqueue noattr))
                       (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                     (Etempvar _thread_id tlong) ::
                     (Etempvar _scratch (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                     nil))
                  (Sset _rc (Etempvar _t'3 tint)))
                (Ssequence
                  (Sifthenelse (Ebinop One (Etempvar _rc tint)
                                 (Econst_int (Int.repr 0) tint) tint)
                    (Ssequence
                      (Scall (Some _t'4)
                        (Evar _map_eevdf_rc (Tfunction (tint :: nil) tint
                                              cc_default))
                        ((Etempvar _rc tint) :: nil))
                      (Sreturn (Some (Etempvar _t'4 tint))))
                    Sskip)
                  (Ssequence
                    (Scall None
                      (Evar _pacha_eevdf_copy_runqueue (Tfunction
                                                         ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                                          (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                                          nil) tvoid
                                                         cc_default))
                      ((Etempvar _scratch (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                       (Eaddrof
                         (Efield
                           (Ederef
                             (Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr)))
                             (Tstruct _pacha_sched_state noattr)) _runqueue
                           (Tstruct _pacha_eevdf_runqueue noattr))
                         (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                       nil))
                    (Ssequence
                      (Scall None
                        (Evar _clear_cpu_current (Tfunction
                                                   ((tptr (Tstruct _pacha_sched_cpu noattr)) ::
                                                    nil) tvoid cc_default))
                        ((Etempvar _cpu (tptr (Tstruct _pacha_sched_cpu noattr))) ::
                         nil))
                      (Sreturn (Some (Econst_int (Int.repr 0) tint))))))))))
        Sskip))
    (Sreturn (Some (Econst_int (Int.repr 0) tint)))))
|}.

Definition f_pacha_sched_finish_current := {|
  fn_return := tint;
  fn_callconv := cc_default;
  fn_params := ((_sched, (tptr (Tstruct _pacha_sched_state noattr))) ::
                (_cpu_id, tulong) ::
                (_decision_out,
                 (tptr (Tstruct _pacha_sched_decision noattr))) ::
                (_scratch, (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
                nil);
  fn_vars := nil;
  fn_temps := ((_t'1, tint) :: (_t'2, tulong) :: nil);
  fn_body :=
(Ssequence
  (Scall None
    (Evar _pacha_sched_no_decision (Tfunction
                                     ((tptr (Tstruct _pacha_sched_decision noattr)) ::
                                      nil) tvoid cc_default))
    ((Etempvar _decision_out (tptr (Tstruct _pacha_sched_decision noattr))) ::
     nil))
  (Ssequence
    (Ssequence
      (Sset _t'2
        (Efield
          (Ederef
            (Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr)))
            (Tstruct _pacha_sched_state noattr)) _cpu_count tulong))
      (Sifthenelse (Ebinop Olt (Etempvar _cpu_id tulong)
                     (Etempvar _t'2 tulong) tint)
        (Ssequence
          (Scall (Some _t'1)
            (Evar _finish_current_valid (Tfunction
                                          ((tptr (Tstruct _pacha_sched_state noattr)) ::
                                           tulong ::
                                           (tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
                                           nil) tint cc_default))
            ((Etempvar _sched (tptr (Tstruct _pacha_sched_state noattr))) ::
             (Etempvar _cpu_id tulong) ::
             (Etempvar _scratch (tptr (Tstruct _pacha_eevdf_runqueue noattr))) ::
             nil))
          (Sreturn (Some (Etempvar _t'1 tint))))
        Sskip))
    (Sreturn (Some (Econst_int (Int.repr 1) tint)))))
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
   noattr ::
 Composite _pacha_sched_decision Struct
   (Member_plain _kind tint :: Member_plain _cpu_id tulong ::
    Member_plain _thread_id tlong :: Member_plain _generation tlong :: nil)
   noattr ::
 Composite _pacha_sched_cpu Struct
   (Member_plain _has_current tint ::
    Member_plain _current_thread_id tlong ::
    Member_plain _current_generation tlong :: nil)
   noattr ::
 Composite _pacha_sched_state Struct
   (Member_plain _runqueue (Tstruct _pacha_eevdf_runqueue noattr) ::
    Member_plain _cpus (tarray (Tstruct _pacha_sched_cpu noattr) 256) ::
    Member_plain _cpu_count tulong :: nil)
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
 (_pacha_eevdf_empty_runqueue,
   Gfun(External (EF_external "pacha_eevdf_empty_runqueue"
                   (mksignature (AST.Xptr :: nil) AST.Xvoid cc_default))
     ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) :: nil) tvoid
     cc_default)) ::
 (_pacha_eevdf_copy_runqueue,
   Gfun(External (EF_external "pacha_eevdf_copy_runqueue"
                   (mksignature (AST.Xptr :: AST.Xptr :: nil) AST.Xvoid
                     cc_default))
     ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
      (tptr (Tstruct _pacha_eevdf_runqueue noattr)) :: nil) tvoid
     cc_default)) ::
 (_pacha_eevdf_add,
   Gfun(External (EF_external "pacha_eevdf_add"
                   (mksignature
                     (AST.Xptr :: AST.Xlong :: AST.Xlong :: AST.Xlong ::
                      AST.Xlong :: AST.Xptr :: nil) AST.Xint cc_default))
     ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) :: tlong :: tlong ::
      tlong :: tlong :: (tptr (Tstruct _pacha_eevdf_runqueue noattr)) :: nil)
     tint cc_default)) ::
 (_pacha_eevdf_wake,
   Gfun(External (EF_external "pacha_eevdf_wake"
                   (mksignature (AST.Xptr :: AST.Xlong :: AST.Xptr :: nil)
                     AST.Xint cc_default))
     ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) :: tlong ::
      (tptr (Tstruct _pacha_eevdf_runqueue noattr)) :: nil) tint cc_default)) ::
 (_pacha_eevdf_block,
   Gfun(External (EF_external "pacha_eevdf_block"
                   (mksignature (AST.Xptr :: AST.Xlong :: AST.Xptr :: nil)
                     AST.Xint cc_default))
     ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) :: tlong ::
      (tptr (Tstruct _pacha_eevdf_runqueue noattr)) :: nil) tint cc_default)) ::
 (_pacha_eevdf_exit,
   Gfun(External (EF_external "pacha_eevdf_exit"
                   (mksignature (AST.Xptr :: AST.Xlong :: AST.Xptr :: nil)
                     AST.Xint cc_default))
     ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) :: tlong ::
      (tptr (Tstruct _pacha_eevdf_runqueue noattr)) :: nil) tint cc_default)) ::
 (_pacha_eevdf_charge,
   Gfun(External (EF_external "pacha_eevdf_charge"
                   (mksignature
                     (AST.Xptr :: AST.Xlong :: AST.Xlong :: AST.Xptr :: nil)
                     AST.Xint cc_default))
     ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) :: tlong :: tlong ::
      (tptr (Tstruct _pacha_eevdf_runqueue noattr)) :: nil) tint cc_default)) ::
 (_pacha_eevdf_mark_running,
   Gfun(External (EF_external "pacha_eevdf_mark_running"
                   (mksignature (AST.Xptr :: AST.Xlong :: AST.Xptr :: nil)
                     AST.Xint cc_default))
     ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) :: tlong ::
      (tptr (Tstruct _pacha_eevdf_runqueue noattr)) :: nil) tint cc_default)) ::
 (_pacha_eevdf_requeue_running,
   Gfun(External (EF_external "pacha_eevdf_requeue_running"
                   (mksignature (AST.Xptr :: AST.Xlong :: AST.Xptr :: nil)
                     AST.Xint cc_default))
     ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) :: tlong ::
      (tptr (Tstruct _pacha_eevdf_runqueue noattr)) :: nil) tint cc_default)) ::
 (_pacha_eevdf_pick,
   Gfun(External (EF_external "pacha_eevdf_pick"
                   (mksignature (AST.Xptr :: AST.Xptr :: nil) AST.Xint
                     cc_default))
     ((tptr (Tstruct _pacha_eevdf_runqueue noattr)) ::
      (tptr (Tstruct _pacha_eevdf_pick_result noattr)) :: nil) tint
     cc_default)) :: (_map_eevdf_rc, Gfun(Internal f_map_eevdf_rc)) ::
 (_pacha_sched_no_decision, Gfun(Internal f_pacha_sched_no_decision)) ::
 (_run_thread_decision, Gfun(Internal f_run_thread_decision)) ::
 (_idle_decision, Gfun(Internal f_idle_decision)) ::
 (_valid_cpu, Gfun(Internal f_valid_cpu)) ::
 (_sched_cpu_const_ptr, Gfun(Internal f_sched_cpu_const_ptr)) ::
 (_sched_cpu_ptr, Gfun(Internal f_sched_cpu_ptr)) ::
 (_sched_cpu_has_current, Gfun(Internal f_sched_cpu_has_current)) ::
 (_cpu_has_current, Gfun(Internal f_cpu_has_current)) ::
 (_clear_cpu_current, Gfun(Internal f_clear_cpu_current)) ::
 (_set_cpu_current, Gfun(Internal f_set_cpu_current)) ::
 (_clear_current_if_matches, Gfun(Internal f_clear_current_if_matches)) ::
 (_current_entity_index, Gfun(Internal f_current_entity_index)) ::
 (_pacha_sched_empty_state, Gfun(Internal f_pacha_sched_empty_state)) ::
 (_pacha_sched_add_thread, Gfun(Internal f_pacha_sched_add_thread)) ::
 (_pacha_sched_wake_thread, Gfun(Internal f_pacha_sched_wake_thread)) ::
 (_pacha_sched_block_thread, Gfun(Internal f_pacha_sched_block_thread)) ::
 (_pacha_sched_exit_thread, Gfun(Internal f_pacha_sched_exit_thread)) ::
 (_on_timer_valid, Gfun(Internal f_on_timer_valid)) ::
 (_pacha_sched_on_timer, Gfun(Internal f_pacha_sched_on_timer)) ::
 (_pacha_sched_pick, Gfun(Internal f_pacha_sched_pick)) ::
 (_finish_current_valid, Gfun(Internal f_finish_current_valid)) ::
 (_pacha_sched_finish_current, Gfun(Internal f_pacha_sched_finish_current)) ::
 nil).

Definition public_idents : list ident :=
(_pacha_sched_finish_current :: _pacha_sched_pick :: _pacha_sched_on_timer ::
 _pacha_sched_exit_thread :: _pacha_sched_block_thread ::
 _pacha_sched_wake_thread :: _pacha_sched_add_thread ::
 _pacha_sched_empty_state :: _pacha_sched_no_decision :: _pacha_eevdf_pick ::
 _pacha_eevdf_requeue_running :: _pacha_eevdf_mark_running ::
 _pacha_eevdf_charge :: _pacha_eevdf_exit :: _pacha_eevdf_block ::
 _pacha_eevdf_wake :: _pacha_eevdf_add :: _pacha_eevdf_copy_runqueue ::
 _pacha_eevdf_empty_runqueue :: ___builtin_debug ::
 ___builtin_write32_reversed :: ___builtin_write16_reversed ::
 ___builtin_read32_reversed :: ___builtin_read16_reversed ::
 ___builtin_fnmsub :: ___builtin_fnmadd :: ___builtin_fmsub ::
 ___builtin_fmadd :: ___builtin_fmin :: ___builtin_fmax ::
 ___builtin_expect :: ___builtin_unreachable :: ___builtin_va_end ::
 ___builtin_va_copy :: ___builtin_va_arg :: ___builtin_va_start ::
 ___builtin_membar :: ___builtin_annot_intval :: ___builtin_annot ::
 ___builtin_sel :: ___builtin_memcpy_aligned :: ___builtin_sqrt ::
 ___builtin_fsqrt :: ___builtin_fabsf :: ___builtin_fabs ::
 ___builtin_ctzll :: ___builtin_ctzl :: ___builtin_ctz :: ___builtin_clzll ::
 ___builtin_clzl :: ___builtin_clz :: ___builtin_bswap16 ::
 ___builtin_bswap32 :: ___builtin_bswap :: ___builtin_bswap64 ::
 ___builtin_ais_annot :: ___compcert_i64_umulh :: ___compcert_i64_smulh ::
 ___compcert_i64_sar :: ___compcert_i64_shr :: ___compcert_i64_shl ::
 ___compcert_i64_umod :: ___compcert_i64_smod :: ___compcert_i64_udiv ::
 ___compcert_i64_sdiv :: ___compcert_i64_utof :: ___compcert_i64_stof ::
 ___compcert_i64_utod :: ___compcert_i64_stod :: ___compcert_i64_dtou ::
 ___compcert_i64_dtos :: ___compcert_va_composite ::
 ___compcert_va_float64 :: ___compcert_va_int64 :: ___compcert_va_int32 ::
 nil).

Definition prog : Clight.program := 
  mkprogram composites global_definitions public_idents _main Logic.I.


