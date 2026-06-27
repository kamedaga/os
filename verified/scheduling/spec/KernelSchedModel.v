From Stdlib Require Import Arith.PeanoNat Bool.Bool Lists.List ZArith.ZArith.
From Pacha.Scheduling Require Import
  ProtocolModel
  EevdfModel
  EevdfPick
  EevdfTransitions.

Import ListNotations.
Open Scope Z_scope.

Definition kernel_sched_max_cpus : nat := 256%nat.
Definition kernel_sched_no_cpu : nat := kernel_sched_max_cpus.

Inductive kernel_sched_rc : Type :=
| KernelSchedOk
| KernelSchedErrInvalid
| KernelSchedErrFull
| KernelSchedErrOverflow
| KernelSchedErrState.

Inductive kernel_sched_decision_kind : Type :=
| KernelDecisionNone
| KernelDecisionRunThread
| KernelDecisionIdle.

Record kernel_sched_decision : Type := {
  ksd_kind : kernel_sched_decision_kind;
  ksd_cpu_id : nat;
  ksd_thread_id : Z;
  ksd_generation : Z;
}.

Record kernel_sched_result : Type := {
  ksr_rc : kernel_sched_rc;
  ksr_decision : kernel_sched_decision;
}.

Record kernel_cpu : Type := {
  kc_has_current : bool;
  kc_current_thread_id : Z;
  kc_current_generation : Z;
  kc_activation_pending : bool;
}.

Record kernel_sched_state : Type := {
  ks_runqueues : list eevdf_runqueue;
  ks_cpus : list kernel_cpu;
  ks_cpu_count : nat;
  ks_balance_cursor : nat;
}.

Definition kernel_empty_cpu : kernel_cpu :=
  {|
    kc_has_current := false;
    kc_current_thread_id := no_thread_id;
    kc_current_generation := 0;
    kc_activation_pending := false;
  |}.

Definition kernel_no_decision : kernel_sched_decision :=
  {|
    ksd_kind := KernelDecisionNone;
    ksd_cpu_id := kernel_sched_no_cpu;
    ksd_thread_id := no_thread_id;
    ksd_generation := 0;
  |}.

Definition kernel_idle_decision
    (cpu_id : nat)
  : kernel_sched_decision :=
  {|
    ksd_kind := KernelDecisionIdle;
    ksd_cpu_id := cpu_id;
    ksd_thread_id := no_thread_id;
    ksd_generation := 0;
  |}.

Definition kernel_run_thread_decision
    (cpu_id : nat)
    (entity : eevdf_entity)
  : kernel_sched_decision :=
  {|
    ksd_kind := KernelDecisionRunThread;
    ksd_cpu_id := cpu_id;
    ksd_thread_id := ee_thread_id entity;
    ksd_generation := ee_generation entity;
  |}.

Definition kernel_sched_ok
    (decision : kernel_sched_decision)
  : kernel_sched_result :=
  {|
    ksr_rc := KernelSchedOk;
    ksr_decision := decision;
  |}.

Definition kernel_sched_fail
    (rc : kernel_sched_rc)
  : kernel_sched_result :=
  {|
    ksr_rc := rc;
    ksr_decision := kernel_no_decision;
  |}.

Definition map_kernel_eevdf_rc
    (rc : eevdf_rc)
  : kernel_sched_rc :=
  match rc with
  | EevdfOk => KernelSchedOk
  | EevdfErrInvalid => KernelSchedErrInvalid
  | EevdfErrFull => KernelSchedErrFull
  | EevdfErrOverflow => KernelSchedErrOverflow
  | EevdfErrState => KernelSchedErrState
  end.

Definition kernel_sched_empty_state
    (cpu_count : nat)
  : kernel_sched_state :=
  let clamped_count :=
    if (kernel_sched_max_cpus <? cpu_count)%nat
    then kernel_sched_max_cpus
    else cpu_count
  in
  {|
    ks_runqueues := repeat eevdf_empty_runqueue kernel_sched_max_cpus;
    ks_cpus := repeat kernel_empty_cpu kernel_sched_max_cpus;
    ks_cpu_count := clamped_count;
    ks_balance_cursor := 0%nat;
  |}.

Definition kernel_valid_cpu
    (sched : kernel_sched_state)
    (cpu_id : nat)
  : bool :=
  (cpu_id <? ks_cpu_count sched)%nat.

Definition kernel_lookup_runqueue
    (sched : kernel_sched_state)
    (cpu_id : nat)
  : option eevdf_runqueue :=
  nth_error (ks_runqueues sched) cpu_id.

Definition kernel_lookup_cpu
    (sched : kernel_sched_state)
    (cpu_id : nat)
  : option kernel_cpu :=
  nth_error (ks_cpus sched) cpu_id.

Definition kernel_replace_runqueue
    (sched : kernel_sched_state)
    (cpu_id : nat)
    (rq : eevdf_runqueue)
  : kernel_sched_state :=
  {|
    ks_runqueues := replace_nth (ks_runqueues sched) cpu_id rq;
    ks_cpus := ks_cpus sched;
    ks_cpu_count := ks_cpu_count sched;
    ks_balance_cursor := ks_balance_cursor sched;
  |}.

Definition kernel_replace_cpu
    (sched : kernel_sched_state)
    (cpu_id : nat)
    (cpu : kernel_cpu)
  : kernel_sched_state :=
  {|
    ks_runqueues := ks_runqueues sched;
    ks_cpus := replace_nth (ks_cpus sched) cpu_id cpu;
    ks_cpu_count := ks_cpu_count sched;
    ks_balance_cursor := ks_balance_cursor sched;
  |}.

Definition kernel_with_balance_cursor
    (sched : kernel_sched_state)
    (cursor : nat)
  : kernel_sched_state :=
  {|
    ks_runqueues := ks_runqueues sched;
    ks_cpus := ks_cpus sched;
    ks_cpu_count := ks_cpu_count sched;
    ks_balance_cursor := cursor;
  |}.

Definition kernel_cpu_has_current
    (sched : kernel_sched_state)
    (cpu_id : nat)
  : bool :=
  if kernel_valid_cpu sched cpu_id then
    match kernel_lookup_cpu sched cpu_id with
    | Some cpu => kc_has_current cpu
    | None => false
    end
  else false.

Definition kernel_cpu_current
    (sched : kernel_sched_state)
    (cpu_id : nat)
  : option (Z * Z) :=
  if kernel_cpu_has_current sched cpu_id then
    match kernel_lookup_cpu sched cpu_id with
    | Some cpu => Some (kc_current_thread_id cpu, kc_current_generation cpu)
    | None => None
    end
  else None.

Fixpoint kernel_current_entity_index_from
    (entities : list eevdf_entity)
    (remaining : nat)
    (thread_id generation : Z)
    (base : nat)
  : option nat :=
  match entities, remaining with
  | [], _ => None
  | _, O => None
  | entity :: rest, S remaining' =>
      if andb
        (andb
          (ee_thread_id entity =? thread_id)
          (ee_generation entity =? generation))
        (match ee_state entity with
         | ERunning => true
         | _ => false
         end)
      then Some base
      else kernel_current_entity_index_from
        rest
        remaining'
        thread_id
        generation
        (S base)
  end.

Definition kernel_current_entity_index
    (rq : eevdf_runqueue)
    (thread_id generation : Z)
  : option nat :=
  kernel_current_entity_index_from
    (er_entities rq)
    (er_entity_count rq)
    thread_id
    generation
    0%nat.

Definition kernel_clear_cpu_current
    (sched : kernel_sched_state)
    (cpu_id : nat)
  : kernel_sched_state :=
  match kernel_lookup_cpu sched cpu_id with
  | Some cpu =>
      kernel_replace_cpu sched cpu_id
        {|
          kc_has_current := false;
          kc_current_thread_id := no_thread_id;
          kc_current_generation := 0;
          kc_activation_pending := kc_activation_pending cpu;
        |}
  | None => sched
  end.

Definition kernel_set_cpu_current
    (sched : kernel_sched_state)
    (cpu_id : nat)
    (thread_id generation : Z)
  : kernel_sched_state :=
  match kernel_lookup_cpu sched cpu_id with
  | Some cpu =>
      kernel_replace_cpu sched cpu_id
        {|
          kc_has_current := true;
          kc_current_thread_id := thread_id;
          kc_current_generation := generation;
          kc_activation_pending := kc_activation_pending cpu;
        |}
  | None => sched
  end.

Definition kernel_set_activation_pending
    (sched : kernel_sched_state)
    (cpu_id : nat)
    (pending : bool)
  : kernel_sched_state :=
  match kernel_lookup_cpu sched cpu_id with
  | Some cpu =>
      kernel_replace_cpu sched cpu_id
        {|
          kc_has_current := kc_has_current cpu;
          kc_current_thread_id := kc_current_thread_id cpu;
          kc_current_generation := kc_current_generation cpu;
          kc_activation_pending := pending;
        |}
  | None => sched
  end.

Fixpoint kernel_clear_current_if_matches_from
    (cpus : list kernel_cpu)
    (thread_id : Z)
  : list kernel_cpu :=
  match cpus with
  | [] => []
  | cpu :: rest =>
      let cpu' :=
        if andb (kc_has_current cpu)
          (Z.eqb (kc_current_thread_id cpu) thread_id)
        then {|
          kc_has_current := false;
          kc_current_thread_id := no_thread_id;
          kc_current_generation := 0;
          kc_activation_pending := kc_activation_pending cpu;
        |}
        else cpu
      in
      cpu' :: kernel_clear_current_if_matches_from rest thread_id
  end.

Definition kernel_clear_current_if_matches
    (sched : kernel_sched_state)
    (thread_id : Z)
  : kernel_sched_state :=
  {|
    ks_runqueues := ks_runqueues sched;
    ks_cpus := kernel_clear_current_if_matches_from (ks_cpus sched) thread_id;
    ks_cpu_count := ks_cpu_count sched;
    ks_balance_cursor := ks_balance_cursor sched;
  |}.

Fixpoint kernel_find_entity_cpu_from
    (runqueues : list eevdf_runqueue)
    (thread_id : Z)
    (cpu_id : nat)
  : option nat :=
  match runqueues with
  | [] => None
  | rq :: rest =>
      match find_entity_index rq thread_id with
      | Some _ => Some cpu_id
      | None => kernel_find_entity_cpu_from rest thread_id (S cpu_id)
      end
  end.

Definition kernel_find_entity_cpu
    (sched : kernel_sched_state)
    (thread_id : Z)
  : option nat :=
  kernel_find_entity_cpu_from
    (firstn (ks_cpu_count sched) (ks_runqueues sched))
    thread_id
    0%nat.

Definition kernel_apply_eevdf_result_to_cpu
    (sched : kernel_sched_state)
    (cpu_id : nat)
    (result : eevdf_result)
  : kernel_sched_state * kernel_sched_result :=
  match eevdf_result_rc result with
  | EevdfOk =>
      (kernel_replace_runqueue sched cpu_id (eevdf_result_rq result),
       kernel_sched_ok kernel_no_decision)
  | rc => (sched, kernel_sched_fail (map_kernel_eevdf_rc rc))
  end.

Definition kernel_add_thread
    (sched : kernel_sched_state)
    (cpu_id : nat)
    (thread_id generation weight slice_ns : Z)
  : kernel_sched_state * kernel_sched_result :=
  if negb (kernel_valid_cpu sched cpu_id) then
    (sched, kernel_sched_fail KernelSchedErrInvalid)
  else
    match kernel_find_entity_cpu sched thread_id with
    | Some _ => (sched, kernel_sched_fail KernelSchedErrInvalid)
    | None =>
        match kernel_lookup_runqueue sched cpu_id with
        | None => (sched, kernel_sched_fail KernelSchedErrInvalid)
        | Some rq =>
            kernel_apply_eevdf_result_to_cpu sched cpu_id
              (eevdf_add rq thread_id generation weight slice_ns)
        end
    end.

Definition kernel_wake_thread
    (sched : kernel_sched_state)
    (thread_id : Z)
  : kernel_sched_state * kernel_sched_result :=
  match kernel_find_entity_cpu sched thread_id with
  | None => (sched, kernel_sched_fail KernelSchedErrInvalid)
  | Some cpu_id =>
      match kernel_lookup_runqueue sched cpu_id with
      | None => (sched, kernel_sched_fail KernelSchedErrInvalid)
      | Some rq =>
          kernel_apply_eevdf_result_to_cpu sched cpu_id
            (eevdf_wake rq thread_id)
      end
  end.

Definition kernel_block_thread
    (sched : kernel_sched_state)
    (thread_id : Z)
  : kernel_sched_state * kernel_sched_result :=
  match kernel_find_entity_cpu sched thread_id with
  | None => (sched, kernel_sched_fail KernelSchedErrInvalid)
  | Some cpu_id =>
      match kernel_lookup_runqueue sched cpu_id with
      | None => (sched, kernel_sched_fail KernelSchedErrInvalid)
      | Some rq =>
          match eevdf_block rq thread_id with
          | {| eevdf_result_rc := EevdfOk; eevdf_result_rq := rq' |} =>
              (kernel_clear_current_if_matches
                 (kernel_replace_runqueue sched cpu_id rq')
                 thread_id,
               kernel_sched_ok kernel_no_decision)
          | {| eevdf_result_rc := rc |} =>
              (sched, kernel_sched_fail (map_kernel_eevdf_rc rc))
          end
      end
  end.

Definition kernel_exit_thread
    (sched : kernel_sched_state)
    (thread_id : Z)
  : kernel_sched_state * kernel_sched_result :=
  match kernel_find_entity_cpu sched thread_id with
  | None => (sched, kernel_sched_fail KernelSchedErrInvalid)
  | Some cpu_id =>
      match kernel_lookup_runqueue sched cpu_id with
      | None => (sched, kernel_sched_fail KernelSchedErrInvalid)
      | Some rq =>
          match eevdf_exit rq thread_id with
          | {| eevdf_result_rc := EevdfOk; eevdf_result_rq := rq' |} =>
              (kernel_clear_current_if_matches
                 (kernel_replace_runqueue sched cpu_id rq')
                 thread_id,
               kernel_sched_ok kernel_no_decision)
          | {| eevdf_result_rc := rc |} =>
              (sched, kernel_sched_fail (map_kernel_eevdf_rc rc))
          end
      end
  end.

Definition kernel_on_timer
    (sched : kernel_sched_state)
    (cpu_id : nat)
    (runtime_ns : Z)
  : kernel_sched_state * kernel_sched_result :=
  if negb (kernel_valid_cpu sched cpu_id) then
    (sched, kernel_sched_fail KernelSchedErrInvalid)
  else
    match kernel_cpu_current sched cpu_id with
    | None => (sched, kernel_sched_ok kernel_no_decision)
    | Some (thread_id, _generation) =>
        match kernel_lookup_runqueue sched cpu_id with
        | None => (sched, kernel_sched_fail KernelSchedErrInvalid)
        | Some rq =>
            match kernel_current_entity_index rq thread_id _generation with
            | None =>
                (kernel_clear_cpu_current sched cpu_id,
                 kernel_sched_ok kernel_no_decision)
            | Some _ =>
                kernel_apply_eevdf_result_to_cpu sched cpu_id
                  (eevdf_charge rq thread_id runtime_ns)
            end
        end
    end.

Definition kernel_pick_cpu
    (sched : kernel_sched_state)
    (cpu_id : nat)
  : kernel_sched_state * kernel_sched_result :=
  if negb (kernel_valid_cpu sched cpu_id) then
    (sched, kernel_sched_fail KernelSchedErrInvalid)
  else if kernel_cpu_has_current sched cpu_id then
    (sched, kernel_sched_fail KernelSchedErrState)
  else
    match kernel_lookup_runqueue sched cpu_id with
    | None => (sched, kernel_sched_fail KernelSchedErrInvalid)
    | Some rq =>
        let '(rq_after_pick, picked) := eevdf_pick rq in
        let sched_after_pick := kernel_replace_runqueue sched cpu_id rq_after_pick in
        match picked with
        | None =>
            (kernel_set_activation_pending sched_after_pick cpu_id false,
             kernel_sched_ok (kernel_idle_decision cpu_id))
        | Some (_index, entity) =>
            match eevdf_mark_running rq_after_pick (ee_thread_id entity) with
            | {| eevdf_result_rc := EevdfOk; eevdf_result_rq := rq' |} =>
                (kernel_set_cpu_current
                   (kernel_set_activation_pending
                     (kernel_replace_runqueue sched_after_pick cpu_id rq')
                     cpu_id
                     false)
                   cpu_id
                   (ee_thread_id entity)
                   (ee_generation entity),
                 kernel_sched_ok (kernel_run_thread_decision cpu_id entity))
            | {| eevdf_result_rc := rc |} =>
                (sched_after_pick, kernel_sched_fail (map_kernel_eevdf_rc rc))
            end
        end
    end.

Definition kernel_finish_current
    (sched : kernel_sched_state)
    (cpu_id : nat)
  : kernel_sched_state * kernel_sched_result :=
  if negb (kernel_valid_cpu sched cpu_id) then
    (sched, kernel_sched_fail KernelSchedErrInvalid)
  else
    match kernel_cpu_current sched cpu_id with
    | None => (sched, kernel_sched_ok kernel_no_decision)
    | Some (thread_id, _generation) =>
        match kernel_lookup_runqueue sched cpu_id with
        | None => (sched, kernel_sched_fail KernelSchedErrInvalid)
        | Some rq =>
            match kernel_current_entity_index rq thread_id _generation with
            | None =>
                (kernel_clear_cpu_current sched cpu_id,
                 kernel_sched_ok kernel_no_decision)
            | Some _ =>
                match eevdf_requeue_running rq thread_id with
                | {| eevdf_result_rc := EevdfOk; eevdf_result_rq := rq' |} =>
                    (kernel_clear_cpu_current
                       (kernel_replace_runqueue sched cpu_id rq')
                       cpu_id,
                     kernel_sched_ok kernel_no_decision)
                | {| eevdf_result_rc := rc |} =>
                    (sched, kernel_sched_fail (map_kernel_eevdf_rc rc))
                end
            end
        end
    end.

Definition kernel_handoff_to_thread_on_cpu
    (sched : kernel_sched_state)
    (cpu_id : nat)
    (target_thread_id : Z)
  : kernel_sched_state * kernel_sched_result :=
  if negb (kernel_valid_cpu sched cpu_id) then
    (sched, kernel_sched_fail KernelSchedErrInvalid)
  else
    match kernel_cpu_current sched cpu_id with
    | None => (sched, kernel_sched_fail KernelSchedErrState)
    | Some (current_thread_id, current_generation) =>
        match kernel_lookup_runqueue sched cpu_id with
        | None => (sched, kernel_sched_fail KernelSchedErrInvalid)
        | Some rq =>
            match kernel_current_entity_index rq current_thread_id current_generation,
                  find_entity_index rq target_thread_id
            with
            | Some _, Some target_index =>
                match lookup_entity rq target_index with
                | None => (sched, kernel_sched_fail KernelSchedErrInvalid)
                | Some target =>
                    match ee_state target with
                    | ERunnable =>
                        match eevdf_requeue_running rq current_thread_id with
                        | {| eevdf_result_rc := EevdfOk; eevdf_result_rq := rq_after_finish |} =>
                            match eevdf_mark_running rq_after_finish target_thread_id with
                            | {| eevdf_result_rc := EevdfOk; eevdf_result_rq := rq_after_target |} =>
                                (kernel_set_cpu_current
                                  (kernel_replace_runqueue sched cpu_id rq_after_target)
                                  cpu_id
                                  (ee_thread_id target)
                                  (ee_generation target),
                                 kernel_sched_ok (kernel_run_thread_decision cpu_id target))
                            | {| eevdf_result_rc := rc |} =>
                                (sched, kernel_sched_fail (map_kernel_eevdf_rc rc))
                            end
                        | {| eevdf_result_rc := rc |} =>
                            (sched, kernel_sched_fail (map_kernel_eevdf_rc rc))
                        end
                    | _ => (sched, kernel_sched_fail KernelSchedErrState)
                    end
                end
            | None, _ => (sched, kernel_sched_fail KernelSchedErrState)
            | _, None => (sched, kernel_sched_fail KernelSchedErrInvalid)
            end
        end
    end.

Definition kernel_request_activation
    (sched : kernel_sched_state)
    (cpu_id : nat)
  : kernel_sched_state * kernel_sched_result :=
  if negb (kernel_valid_cpu sched cpu_id) then
    (sched, kernel_sched_fail KernelSchedErrInvalid)
  else
    (kernel_set_activation_pending sched cpu_id true,
     kernel_sched_ok kernel_no_decision).

Definition kernel_claim_activation
    (sched : kernel_sched_state)
    (cpu_id : nat)
  : kernel_sched_state * kernel_sched_result :=
  if negb (kernel_valid_cpu sched cpu_id) then
    (sched, kernel_sched_fail KernelSchedErrInvalid)
  else
    kernel_pick_cpu sched cpu_id.

Definition entity_as_migrated_runnable
    (dst : eevdf_runqueue)
    (entity : eevdf_entity)
  : option eevdf_entity :=
  refresh_deadline
    {|
      ee_thread_id := ee_thread_id entity;
      ee_generation := ee_generation entity;
      ee_weight := ee_weight entity;
      ee_slice_ns := ee_slice_ns entity;
      ee_service_ns := ee_service_ns entity;
      ee_vruntime := z_max (ee_vruntime entity) (er_min_vruntime dst);
      ee_eligible_time := ee_eligible_time entity;
      ee_deadline := ee_deadline entity;
      ee_state := ERunnable;
    |}
    (er_min_vruntime dst).

Definition compact_entities
    (entities : list eevdf_entity)
  : list eevdf_entity :=
  firstn eevdf_max_entities
    (entities ++ repeat eevdf_empty_entity eevdf_max_entities).

Definition remove_entity_from_runqueue
    (rq : eevdf_runqueue)
    (thread_id : Z)
  : eevdf_runqueue :=
  let active := firstn (er_entity_count rq) (er_entities rq) in
  let kept :=
    filter
      (fun entity => negb (Z.eqb (ee_thread_id entity) thread_id))
      active
  in
  refresh_runqueue
    {|
      er_entities := compact_entities kept;
      er_entity_count := length kept;
      er_runnable_count := count_runnable kept;
      er_virtual_time := er_virtual_time rq;
      er_min_vruntime := er_min_vruntime rq;
    |}.

Definition kernel_migrate_runnable
    (sched : kernel_sched_state)
    (src_cpu dst_cpu : nat)
    (thread_id : Z)
  : kernel_sched_state * kernel_sched_result :=
  if orb
    (negb (kernel_valid_cpu sched src_cpu))
    (negb (kernel_valid_cpu sched dst_cpu))
  then (sched, kernel_sched_fail KernelSchedErrInvalid)
  else if Nat.eqb src_cpu dst_cpu then
    (sched, kernel_sched_fail KernelSchedErrInvalid)
  else
    match kernel_lookup_runqueue sched src_cpu,
          kernel_lookup_runqueue sched dst_cpu with
    | Some src, Some dst =>
        match find_entity_index src thread_id with
        | None => (sched, kernel_sched_fail KernelSchedErrInvalid)
        | Some index =>
            match lookup_entity src index with
            | None => (sched, kernel_sched_fail KernelSchedErrInvalid)
            | Some entity =>
                match ee_state entity with
                | ERunnable =>
                    if (er_entity_count dst <? eevdf_max_entities)%nat then
                      match find_entity_index dst thread_id with
                      | Some _ => (sched, kernel_sched_fail KernelSchedErrInvalid)
                      | None =>
                          match entity_as_migrated_runnable dst entity with
                          | Some moved =>
                              let src' := remove_entity_from_runqueue src thread_id in
                              let dst' := refresh_runqueue (append_entity dst moved) in
                              (kernel_replace_runqueue
                                (kernel_replace_runqueue sched src_cpu src')
                                dst_cpu
                                dst',
                               kernel_sched_ok kernel_no_decision)
                          | None => (sched, kernel_sched_fail KernelSchedErrOverflow)
                          end
                      end
                    else (sched, kernel_sched_fail KernelSchedErrFull)
                | _ => (sched, kernel_sched_fail KernelSchedErrState)
                end
            end
        end
    | _, _ => (sched, kernel_sched_fail KernelSchedErrInvalid)
    end.
