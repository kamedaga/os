From Stdlib Require Import Arith.PeanoNat Bool.Bool Lists.List ZArith.ZArith.
From Pacha.Scheduling Require Import
  ProtocolModel
  EevdfModel
  EevdfPick
  EevdfTransitions.

Import ListNotations.
Open Scope Z_scope.

Definition sched_max_cpus : nat := 256%nat.

Inductive sched_rc : Type :=
| SchedOk
| SchedErrInvalid
| SchedErrFull
| SchedErrOverflow
| SchedErrState.

Inductive sched_decision_kind : Type :=
| SchedDecisionNone
| SchedDecisionRunThread
| SchedDecisionIdle.

Record sched_decision : Type := {
  sd_kind : sched_decision_kind;
  sd_cpu_id : nat;
  sd_thread_id : Z;
  sd_generation : Z;
}.

Record sched_result : Type := {
  sr_rc : sched_rc;
  sr_decision : sched_decision;
}.

Record sched_cpu : Type := {
  sc_has_current : bool;
  sc_current_thread_id : Z;
  sc_current_generation : Z;
}.

Record sched_state : Type := {
  ss_runqueue : eevdf_runqueue;
  ss_cpus : list sched_cpu;
  ss_cpu_count : nat;
}.

Definition sched_no_cpu : nat := sched_max_cpus.

Definition sched_no_decision : sched_decision :=
  {|
    sd_kind := SchedDecisionNone;
    sd_cpu_id := sched_no_cpu;
    sd_thread_id := no_thread_id;
    sd_generation := 0;
  |}.

Definition sched_idle_decision
    (cpu_id : nat)
  : sched_decision :=
  {|
    sd_kind := SchedDecisionIdle;
    sd_cpu_id := cpu_id;
    sd_thread_id := no_thread_id;
    sd_generation := 0;
  |}.

Definition sched_run_thread_decision
    (cpu_id : nat)
    (entity : eevdf_entity)
  : sched_decision :=
  {|
    sd_kind := SchedDecisionRunThread;
    sd_cpu_id := cpu_id;
    sd_thread_id := ee_thread_id entity;
    sd_generation := ee_generation entity;
  |}.

Definition sched_ok
    (decision : sched_decision)
  : sched_result :=
  {|
    sr_rc := SchedOk;
    sr_decision := decision;
  |}.

Definition sched_fail
    (rc : sched_rc)
  : sched_result :=
  {|
    sr_rc := rc;
    sr_decision := sched_no_decision;
  |}.

Definition map_eevdf_rc
    (rc : eevdf_rc)
  : sched_rc :=
  match rc with
  | EevdfOk => SchedOk
  | EevdfErrInvalid => SchedErrInvalid
  | EevdfErrFull => SchedErrFull
  | EevdfErrOverflow => SchedErrOverflow
  | EevdfErrState => SchedErrState
  end.

Definition sched_empty_cpu : sched_cpu :=
  {|
    sc_has_current := false;
    sc_current_thread_id := no_thread_id;
    sc_current_generation := 0;
  |}.

Definition sched_empty_state
    (cpu_count : nat)
  : sched_state :=
  let clamped_count :=
    if (sched_max_cpus <? cpu_count)%nat then sched_max_cpus else cpu_count
  in
  {|
    ss_runqueue := eevdf_empty_runqueue;
    ss_cpus := repeat sched_empty_cpu sched_max_cpus;
    ss_cpu_count := clamped_count;
  |}.

Definition valid_cpu
    (sched : sched_state)
    (cpu_id : nat)
  : bool :=
  (cpu_id <? ss_cpu_count sched)%nat.

Definition lookup_cpu
    (sched : sched_state)
    (cpu_id : nat)
  : option sched_cpu :=
  nth_error (ss_cpus sched) cpu_id.

Definition cpu_has_current
    (sched : sched_state)
    (cpu_id : nat)
  : bool :=
  if valid_cpu sched cpu_id then
    match lookup_cpu sched cpu_id with
    | Some cpu => sc_has_current cpu
    | None => false
    end
  else false.

Definition replace_cpu
    (sched : sched_state)
    (cpu_id : nat)
    (cpu : sched_cpu)
  : sched_state :=
  {|
    ss_runqueue := ss_runqueue sched;
    ss_cpus := replace_nth (ss_cpus sched) cpu_id cpu;
    ss_cpu_count := ss_cpu_count sched;
  |}.

Definition with_runqueue
    (sched : sched_state)
    (rq : eevdf_runqueue)
  : sched_state :=
  {|
    ss_runqueue := rq;
    ss_cpus := ss_cpus sched;
    ss_cpu_count := ss_cpu_count sched;
  |}.

Definition clear_cpu_current
    (sched : sched_state)
    (cpu_id : nat)
  : sched_state :=
  replace_cpu sched cpu_id sched_empty_cpu.

Definition set_cpu_current
    (sched : sched_state)
    (cpu_id : nat)
    (thread_id : Z)
    (generation : Z)
  : sched_state :=
  replace_cpu sched cpu_id
    {|
      sc_has_current := true;
      sc_current_thread_id := thread_id;
      sc_current_generation := generation;
    |}.

Definition cpu_current_thread
    (sched : sched_state)
    (cpu_id : nat)
  : option (Z * Z) :=
  if cpu_has_current sched cpu_id then
    match lookup_cpu sched cpu_id with
    | Some cpu => Some (sc_current_thread_id cpu, sc_current_generation cpu)
    | None => None
    end
  else None.

Definition cpu_current_thread_id
    (sched : sched_state)
    (cpu_id : nat)
  : option Z :=
  match cpu_current_thread sched cpu_id with
  | Some (thread_id, _generation) => Some thread_id
  | None => None
  end.

Definition is_running_entity
    (entity : eevdf_entity)
  : bool :=
  match ee_state entity with
  | ERunning => true
  | _ => false
  end.

Definition current_running_thread_id
    (sched : sched_state)
    (cpu_id : nat)
  : option Z :=
  match cpu_current_thread sched cpu_id with
  | None => None
  | Some (thread_id, generation) =>
      match find_entity_index (ss_runqueue sched) thread_id with
      | None => None
      | Some index =>
          let entity := nth index (er_entities (ss_runqueue sched))
            eevdf_empty_entity in
          if andb
            (Z.eqb (ee_generation entity) generation)
            (is_running_entity entity)
          then Some thread_id
          else None
      end
  end.

Fixpoint clear_current_if_matches_from
    (cpus : list sched_cpu)
    (thread_id : Z)
  : list sched_cpu :=
  match cpus with
  | [] => []
  | cpu :: rest =>
      let cpu' :=
        if andb (sc_has_current cpu)
          (Z.eqb (sc_current_thread_id cpu) thread_id)
        then sched_empty_cpu
        else cpu
      in
      cpu' :: clear_current_if_matches_from rest thread_id
  end.

Definition clear_current_if_matches
    (sched : sched_state)
    (thread_id : Z)
  : sched_state :=
  {|
    ss_runqueue := ss_runqueue sched;
    ss_cpus := clear_current_if_matches_from (ss_cpus sched) thread_id;
    ss_cpu_count := ss_cpu_count sched;
  |}.

Definition sched_apply_eevdf_result
    (sched : sched_state)
    (result : eevdf_result)
  : sched_state * sched_result :=
  match eevdf_result_rc result with
  | EevdfOk =>
      (with_runqueue sched (eevdf_result_rq result), sched_ok sched_no_decision)
  | rc => (sched, sched_fail (map_eevdf_rc rc))
  end.

Definition sched_add_thread
    (sched : sched_state)
    (thread_id generation weight slice_ns : Z)
  : sched_state * sched_result :=
  sched_apply_eevdf_result sched
    (eevdf_add (ss_runqueue sched) thread_id generation weight slice_ns).

Definition sched_wake_thread
    (sched : sched_state)
    (thread_id : Z)
  : sched_state * sched_result :=
  sched_apply_eevdf_result sched
    (eevdf_wake (ss_runqueue sched) thread_id).

Definition sched_block_thread
    (sched : sched_state)
    (thread_id : Z)
  : sched_state * sched_result :=
  match eevdf_block (ss_runqueue sched) thread_id with
  | {| eevdf_result_rc := EevdfOk; eevdf_result_rq := rq |} =>
      (clear_current_if_matches (with_runqueue sched rq) thread_id,
       sched_ok sched_no_decision)
  | {| eevdf_result_rc := rc |} =>
      (sched, sched_fail (map_eevdf_rc rc))
  end.

Definition sched_exit_thread
    (sched : sched_state)
    (thread_id : Z)
  : sched_state * sched_result :=
  match eevdf_exit (ss_runqueue sched) thread_id with
  | {| eevdf_result_rc := EevdfOk; eevdf_result_rq := rq |} =>
      (clear_current_if_matches (with_runqueue sched rq) thread_id,
       sched_ok sched_no_decision)
  | {| eevdf_result_rc := rc |} =>
      (sched, sched_fail (map_eevdf_rc rc))
  end.

Definition sched_on_timer
    (sched : sched_state)
    (cpu_id : nat)
    (runtime_ns : Z)
  : sched_state * sched_result :=
  if negb (valid_cpu sched cpu_id) then
    (sched, sched_fail SchedErrInvalid)
  else
    match cpu_current_thread sched cpu_id with
    | None => (sched, sched_ok sched_no_decision)
    | Some _ =>
        match current_running_thread_id sched cpu_id with
        | None => (clear_cpu_current sched cpu_id, sched_ok sched_no_decision)
        | Some thread_id =>
            sched_apply_eevdf_result sched
              (eevdf_charge (ss_runqueue sched) thread_id runtime_ns)
        end
    end.

Definition sched_pick
    (sched : sched_state)
    (cpu_id : nat)
  : sched_state * sched_result :=
  if negb (valid_cpu sched cpu_id) then
    (sched, sched_fail SchedErrInvalid)
  else if cpu_has_current sched cpu_id then
    (sched, sched_fail SchedErrState)
  else
    let '(rq_after_pick, picked) := eevdf_pick (ss_runqueue sched) in
    let sched_after_pick := with_runqueue sched rq_after_pick in
    match picked with
    | None => (sched_after_pick, sched_ok (sched_idle_decision cpu_id))
    | Some (_index, entity) =>
        match eevdf_mark_running
          (ss_runqueue sched_after_pick)
          (ee_thread_id entity)
        with
        | {| eevdf_result_rc := EevdfOk; eevdf_result_rq := rq |} =>
            (set_cpu_current (with_runqueue sched_after_pick rq) cpu_id
              (ee_thread_id entity)
              (ee_generation entity),
             sched_ok (sched_run_thread_decision cpu_id entity))
        | {| eevdf_result_rc := rc |} =>
            (sched_after_pick, sched_fail (map_eevdf_rc rc))
        end
    end.

Definition sched_finish_current
    (sched : sched_state)
    (cpu_id : nat)
  : sched_state * sched_result :=
  if negb (valid_cpu sched cpu_id) then
    (sched, sched_fail SchedErrInvalid)
  else
    match cpu_current_thread sched cpu_id with
    | None => (sched, sched_ok sched_no_decision)
    | Some _ =>
        match current_running_thread_id sched cpu_id with
        | None => (clear_cpu_current sched cpu_id, sched_ok sched_no_decision)
        | Some thread_id =>
            match eevdf_requeue_running (ss_runqueue sched) thread_id with
            | {| eevdf_result_rc := EevdfOk; eevdf_result_rq := rq |} =>
                (clear_cpu_current (with_runqueue sched rq) cpu_id,
                 sched_ok sched_no_decision)
            | {| eevdf_result_rc := rc |} =>
                (sched, sched_fail (map_eevdf_rc rc))
            end
        end
    end.
