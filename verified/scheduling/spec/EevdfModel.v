From Stdlib Require Import Bool.Bool Lia Lists.List ZArith.ZArith.
From Pacha.Scheduling Require Import ProtocolModel.

Import ListNotations.
Open Scope Z_scope.

Definition eevdf_max_entities : nat := 256%nat.
Definition eevdf_default_weight : Z := 1024.
Definition eevdf_default_slice_ns : Z := 4000000.
Definition i64_max : Z := 9223372036854775807.
Definition no_thread_id : Z := 0.

Inductive eevdf_state : Type :=
| EEmpty
| ERunnable
| ERunning
| EBlocked
| EExited.

Inductive eevdf_rc : Type :=
| EevdfOk
| EevdfErrInvalid
| EevdfErrFull
| EevdfErrOverflow
| EevdfErrState.

Record eevdf_entity : Type := {
  ee_thread_id : Z;
  ee_generation : Z;
  ee_weight : Z;
  ee_slice_ns : Z;
  ee_service_ns : Z;
  ee_vruntime : Z;
  ee_eligible_time : Z;
  ee_deadline : Z;
  ee_state : eevdf_state;
}.

Record eevdf_runqueue : Type := {
  er_entities : list eevdf_entity;
  er_entity_count : nat;
  er_runnable_count : nat;
  er_virtual_time : Z;
  er_min_vruntime : Z;
}.

Record eevdf_result : Type := {
  eevdf_result_rc : eevdf_rc;
  eevdf_result_rq : eevdf_runqueue;
}.

Definition ok
    (rq : eevdf_runqueue)
  : eevdf_result :=
  {|
    eevdf_result_rc := EevdfOk;
    eevdf_result_rq := rq;
  |}.

Definition fail
    (rc : eevdf_rc)
    (rq : eevdf_runqueue)
  : eevdf_result :=
  {|
    eevdf_result_rc := rc;
    eevdf_result_rq := rq;
  |}.

Definition z_max
    (lhs rhs : Z)
  : Z :=
  if lhs <? rhs then rhs else lhs.

Definition clamp_weight
    (weight : Z)
  : Z :=
  if weight =? 0 then eevdf_default_weight else weight.

Definition clamp_slice
    (slice_ns : Z)
  : Z :=
  if slice_ns =? 0 then eevdf_default_slice_ns else slice_ns.

Definition valid_positive
    (value : Z)
  : bool :=
  0 <? value.

Definition i64_nonnegative
    (value : Z)
  : bool :=
  andb (0 <=? value) (value <=? i64_max).

Definition i64_value
    (value : Z)
  : bool :=
  andb ((- i64_max - 1) <=? value) (value <=? i64_max).

Definition weighted_delta
    (runtime_ns : Z)
    (weight : Z)
  : option Z :=
  if andb (i64_nonnegative runtime_ns) (valid_positive weight) then
    let scaled := (runtime_ns * eevdf_default_weight) / weight in
    if scaled <=? i64_max then Some scaled else None
  else None.

Definition weighted_slice
    (slice_ns : Z)
    (weight : Z)
  : option Z :=
  match weighted_delta slice_ns weight with
  | Some delta => Some (z_max 1 delta)
  | None => None
  end.

Definition refresh_deadline
    (entity : eevdf_entity)
    (floor_vruntime : Z)
  : option eevdf_entity :=
  match weighted_slice (ee_slice_ns entity) (ee_weight entity) with
  | Some slice =>
      let eligible := z_max (ee_vruntime entity) floor_vruntime in
      let deadline := eligible + slice in
      if i64_value deadline then
        Some {|
          ee_thread_id := ee_thread_id entity;
          ee_generation := ee_generation entity;
          ee_weight := ee_weight entity;
          ee_slice_ns := ee_slice_ns entity;
          ee_service_ns := ee_service_ns entity;
          ee_vruntime := ee_vruntime entity;
          ee_eligible_time := eligible;
          ee_deadline := deadline;
          ee_state := ee_state entity;
        |}
      else None
  | None => None
  end.

Definition live_for_min
    (entity : eevdf_entity)
  : bool :=
  match ee_state entity with
  | ERunnable => true
  | ERunning => true
  | _ => false
  end.

Definition is_runnable
    (entity : eevdf_entity)
  : bool :=
  match ee_state entity with
  | ERunnable => true
  | _ => false
  end.

Definition runnable_or_running
    (entity : eevdf_entity)
  : bool :=
  match ee_state entity with
  | ERunnable => true
  | ERunning => true
  | _ => false
  end.

Definition is_active_state
    (state : eevdf_state)
  : bool :=
  match state with
  | EEmpty => false
  | _ => true
  end.

Definition count_runnable
    (entities : list eevdf_entity)
  : nat :=
  length (filter is_runnable entities).

Fixpoint min_vruntime_from
    (current : Z)
    (found : bool)
    (entities : list eevdf_entity)
  : Z :=
  match entities with
  | [] => current
  | entity :: rest =>
      if live_for_min entity then
        let next :=
          if found then
            if ee_vruntime entity <? current then ee_vruntime entity else current
          else ee_vruntime entity
        in
        min_vruntime_from next true rest
      else min_vruntime_from current found rest
  end.

Definition computed_min_vruntime
    (rq : eevdf_runqueue)
  : Z :=
  min_vruntime_from (er_min_vruntime rq) false (firstn (er_entity_count rq) (er_entities rq)).

Definition refresh_runqueue
    (rq : eevdf_runqueue)
  : eevdf_runqueue :=
  let active := firstn (er_entity_count rq) (er_entities rq) in
  let min_vruntime := computed_min_vruntime rq in
  {|
    er_entities := er_entities rq;
    er_entity_count := er_entity_count rq;
    er_runnable_count := count_runnable active;
    er_virtual_time := z_max (er_virtual_time rq) min_vruntime;
    er_min_vruntime := min_vruntime;
  |}.

Definition recount_runqueue
    (rq : eevdf_runqueue)
  : eevdf_runqueue :=
  let active := firstn (er_entity_count rq) (er_entities rq) in
  {|
    er_entities := er_entities rq;
    er_entity_count := er_entity_count rq;
    er_runnable_count := count_runnable active;
    er_virtual_time := er_virtual_time rq;
    er_min_vruntime := er_min_vruntime rq;
  |}.

Definition set_entity_state
    (entity : eevdf_entity)
    (state : eevdf_state)
  : eevdf_entity :=
  {|
    ee_thread_id := ee_thread_id entity;
    ee_generation := ee_generation entity;
    ee_weight := ee_weight entity;
    ee_slice_ns := ee_slice_ns entity;
    ee_service_ns := ee_service_ns entity;
    ee_vruntime := ee_vruntime entity;
    ee_eligible_time := ee_eligible_time entity;
    ee_deadline := ee_deadline entity;
    ee_state := state;
  |}.

Definition place_entity_at_floor
    (entity : eevdf_entity)
    (floor_vruntime : Z)
  : eevdf_entity :=
  {|
    ee_thread_id := ee_thread_id entity;
    ee_generation := ee_generation entity;
    ee_weight := ee_weight entity;
    ee_slice_ns := ee_slice_ns entity;
    ee_service_ns := ee_service_ns entity;
    ee_vruntime := z_max (ee_vruntime entity) floor_vruntime;
    ee_eligible_time := ee_eligible_time entity;
    ee_deadline := ee_deadline entity;
    ee_state := ee_state entity;
  |}.

Definition init_entity
    (rq : eevdf_runqueue)
    (thread_id generation weight slice_ns : Z)
  : option eevdf_entity :=
  let entity :=
    {|
      ee_thread_id := thread_id;
      ee_generation := generation;
      ee_weight := clamp_weight weight;
      ee_slice_ns := clamp_slice slice_ns;
      ee_service_ns := 0;
      ee_vruntime := er_min_vruntime rq;
      ee_eligible_time := er_min_vruntime rq;
      ee_deadline := er_min_vruntime rq;
      ee_state := ERunnable;
    |}
  in
  refresh_deadline entity (er_min_vruntime rq).

Definition replace_entity
    (rq : eevdf_runqueue)
    (index : nat)
    (entity : eevdf_entity)
  : eevdf_runqueue :=
  {|
    er_entities := replace_nth (er_entities rq) index entity;
    er_entity_count := er_entity_count rq;
    er_runnable_count := er_runnable_count rq;
    er_virtual_time := er_virtual_time rq;
    er_min_vruntime := er_min_vruntime rq;
  |}.

Fixpoint find_entity_index_from
    (entities : list eevdf_entity)
    (remaining : nat)
    (thread_id : Z)
    (index : nat)
  : option nat :=
  match remaining, entities with
  | O, _ => None
  | _, [] => None
  | S remaining', entity :: rest =>
      if andb
          (ee_thread_id entity =? thread_id)
          (is_active_state (ee_state entity))
      then Some index
      else find_entity_index_from rest remaining' thread_id (S index)
  end.

Definition find_entity_index
    (rq : eevdf_runqueue)
    (thread_id : Z)
  : option nat :=
  if thread_id =? no_thread_id then None
  else find_entity_index_from (er_entities rq) (er_entity_count rq) thread_id 0%nat.

Definition lookup_entity
    (rq : eevdf_runqueue)
    (index : nat)
  : option eevdf_entity :=
  nth_error (er_entities rq) index.

Definition entity_better
    (candidate current : eevdf_entity)
  : bool :=
  orb
    (ee_deadline candidate <? ee_deadline current)
    (andb
      (ee_deadline candidate =? ee_deadline current)
      (ee_thread_id candidate <? ee_thread_id current)).

Fixpoint active_indexed_from
    (entities : list eevdf_entity)
    (remaining : nat)
    (index : nat)
  : list (nat * eevdf_entity) :=
  match remaining, entities with
  | O, _ => []
  | _, [] => []
  | S remaining', entity :: rest =>
      (index, entity) :: active_indexed_from rest remaining' (S index)
  end.

Definition eligible_pair
    (virtual_time : Z)
    (item : nat * eevdf_entity)
  : bool :=
  let (_, entity) := item in
  andb (is_runnable entity) (ee_eligible_time entity <=? virtual_time).

Fixpoint eligible_indexed
    (virtual_time : Z)
    (items : list (nat * eevdf_entity))
  : list (nat * eevdf_entity) :=
  match items with
  | [] => []
  | item :: rest =>
      if eligible_pair virtual_time item then
        item :: eligible_indexed virtual_time rest
      else eligible_indexed virtual_time rest
  end.

Definition pair_entity
    (item : nat * eevdf_entity)
  : eevdf_entity :=
  snd item.

Definition pair_better
    (candidate current : nat * eevdf_entity)
  : bool :=
  entity_better (pair_entity candidate) (pair_entity current).

Fixpoint select_best
    (items : list (nat * eevdf_entity))
  : option (nat * eevdf_entity) :=
  match items with
  | [] => None
  | item :: rest =>
      match select_best rest with
      | None => Some item
      | Some best =>
          if pair_better item best then Some item else Some best
      end
  end.

Definition runnable_pair
    (item : nat * eevdf_entity)
  : bool :=
  is_runnable (pair_entity item).

Fixpoint runnable_indexed
    (items : list (nat * eevdf_entity))
  : list (nat * eevdf_entity) :=
  match items with
  | [] => []
  | item :: rest =>
      if runnable_pair item then
        item :: runnable_indexed rest
      else runnable_indexed rest
  end.

Definition pair_eligible_time
    (item : nat * eevdf_entity)
  : Z :=
  ee_eligible_time (pair_entity item).

Definition pair_earlier
    (candidate current : nat * eevdf_entity)
  : bool :=
  pair_eligible_time candidate <? pair_eligible_time current.

Fixpoint select_earliest
    (items : list (nat * eevdf_entity))
  : option (nat * eevdf_entity) :=
  match items with
  | [] => None
  | item :: rest =>
      match select_earliest rest with
      | None => Some item
      | Some earliest =>
          if pair_earlier item earliest then Some item else Some earliest
      end
  end.

Definition best_eligible_from
    (entities : list eevdf_entity)
    (remaining : nat)
    (virtual_time : Z)
    (index : nat)
  : option (nat * eevdf_entity) :=
  select_best
    (eligible_indexed virtual_time
      (active_indexed_from entities remaining index)).

Fixpoint next_eligible_from
    (entities : list eevdf_entity)
    (remaining : nat)
    (index : nat)
    (best : option Z)
  : option Z :=
  match remaining, entities with
  | O, _ => best
  | _, [] => best
  | S remaining', entity :: rest =>
      let best' :=
        if is_runnable entity then
          match best with
          | None => Some (ee_eligible_time entity)
          | Some current =>
              if ee_eligible_time entity <? current then Some (ee_eligible_time entity)
              else best
          end
        else best
      in
      next_eligible_from rest remaining' (S index) best'
  end.

Definition best_eligible
    (rq : eevdf_runqueue)
    (virtual_time : Z)
  : option (nat * eevdf_entity) :=
  best_eligible_from (er_entities rq) (er_entity_count rq) virtual_time 0%nat.

Definition next_eligible
    (rq : eevdf_runqueue)
  : option Z :=
  match select_earliest
    (runnable_indexed
      (active_indexed_from (er_entities rq) (er_entity_count rq) 0%nat))
  with
  | Some item => Some (pair_eligible_time item)
  | None => None
  end.

Definition pick_result : Type := eevdf_runqueue * option (nat * eevdf_entity).

Definition eevdf_pick
    (rq : eevdf_runqueue)
  : pick_result :=
  match best_eligible rq (er_virtual_time rq) with
  | Some picked => (rq, Some picked)
  | None =>
      match next_eligible rq with
      | None => (rq, None)
      | Some next_time =>
          let rq' :=
            {|
              er_entities := er_entities rq;
              er_entity_count := er_entity_count rq;
              er_runnable_count := er_runnable_count rq;
              er_virtual_time := next_time;
              er_min_vruntime := er_min_vruntime rq;
            |}
          in
          (rq', best_eligible rq' next_time)
      end
  end.

Definition eevdf_charge
    (rq : eevdf_runqueue)
    (thread_id runtime_ns : Z)
  : eevdf_result :=
  match find_entity_index rq thread_id with
  | None => fail EevdfErrInvalid rq
  | Some index =>
      match lookup_entity rq index with
      | None => fail EevdfErrInvalid rq
      | Some entity =>
          if runnable_or_running entity
          then
            match weighted_delta runtime_ns (ee_weight entity) with
            | None => fail EevdfErrOverflow rq
            | Some delta =>
                let vruntime' := ee_vruntime entity + delta in
                if i64_value vruntime' then
                  let entity' :=
                    {|
                      ee_thread_id := ee_thread_id entity;
                      ee_generation := ee_generation entity;
                      ee_weight := ee_weight entity;
                      ee_slice_ns := ee_slice_ns entity;
                      ee_service_ns := ee_service_ns entity + runtime_ns;
                      ee_vruntime := vruntime';
                      ee_eligible_time := ee_eligible_time entity;
                      ee_deadline := ee_deadline entity;
                      ee_state := ee_state entity;
                    |}
                  in
                  match refresh_deadline entity' (er_min_vruntime rq) with
                  | Some refreshed =>
                      ok (refresh_runqueue (replace_entity rq index refreshed))
                  | None => fail EevdfErrOverflow rq
                  end
                else fail EevdfErrOverflow rq
            end
          else fail EevdfErrState rq
      end
  end.
