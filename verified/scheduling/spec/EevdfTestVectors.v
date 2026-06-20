From Stdlib Require Import Lists.List ZArith.ZArith.
From Pacha.Scheduling Require Import EevdfModel EevdfTransitions.

Open Scope Z_scope.

Definition test_add_ok
    (rq : eevdf_runqueue)
    (thread_id generation weight slice_ns : Z)
  : eevdf_runqueue :=
  let result := eevdf_add rq thread_id generation weight slice_ns in
  match eevdf_result_rc result with
  | EevdfOk => eevdf_result_rq result
  | _ => rq
  end.

Fixpoint test_add_range
    (count : nat)
    (rq : eevdf_runqueue)
    (next_thread_id : Z)
  : eevdf_runqueue :=
  match count with
  | O => rq
  | S rest =>
      test_add_range
        rest
        (test_add_ok rq next_thread_id 1 1024 4000000)
        (next_thread_id + 1)
  end.

Definition tv_add_42 : eevdf_result :=
  eevdf_add eevdf_empty_runqueue 42 7 1024 4000000.

Example tv_duplicate_thread :
  eevdf_result_rc
    (eevdf_add (eevdf_result_rq tv_add_42) 42 8 1024 4000000) =
  EevdfErrInvalid.
Proof.
  reflexivity.
Qed.

Definition tv_full_rq : eevdf_runqueue :=
  test_add_range eevdf_max_entities eevdf_empty_runqueue 1.

Example tv_table_full_count :
  er_entity_count tv_full_rq = eevdf_max_entities.
Proof.
  vm_compute.
  reflexivity.
Qed.

Example tv_table_full_rejects_add :
  eevdf_result_rc (eevdf_add tv_full_rq 1000 1 1024 4000000) =
  EevdfErrFull.
Proof.
  vm_compute.
  reflexivity.
Qed.

Definition tv_pick_tie_rq : eevdf_runqueue :=
  test_add_ok
    (test_add_ok eevdf_empty_runqueue 20 1 1024 4000000)
    10
    1
    1024
    4000000.

Example tv_pick_tie_breaks_by_thread_id :
  eevdf_pick tv_pick_tie_rq =
    (tv_pick_tie_rq, Some (1%nat, nth 1 (er_entities tv_pick_tie_rq) eevdf_empty_entity)).
Proof.
  vm_compute.
  reflexivity.
Qed.

Definition tv_block_wake_add_a : eevdf_runqueue :=
  test_add_ok eevdf_empty_runqueue 1 1 1024 4000000.

Definition tv_block_wake_add_b : eevdf_runqueue :=
  test_add_ok tv_block_wake_add_a 2 1 1024 4000000.

Definition tv_block_wake_blocked : eevdf_runqueue :=
  eevdf_result_rq (eevdf_block tv_block_wake_add_b 1).

Definition tv_block_wake_marked : eevdf_runqueue :=
  eevdf_result_rq (eevdf_mark_running tv_block_wake_blocked 2).

Definition tv_block_wake_charged : eevdf_runqueue :=
  eevdf_result_rq (eevdf_charge tv_block_wake_marked 2 5000).

Definition tv_block_wake_woken : eevdf_result :=
  eevdf_wake tv_block_wake_charged 1.

Example tv_blocked_wake_places_at_floor :
  ee_vruntime
    (nth 0 (er_entities (eevdf_result_rq tv_block_wake_woken)) eevdf_empty_entity) =
  5000.
Proof.
  vm_compute.
  reflexivity.
Qed.

Example tv_blocked_wake_sets_eligible_to_floor :
  ee_eligible_time
    (nth 0 (er_entities (eevdf_result_rq tv_block_wake_woken)) eevdf_empty_entity) =
  5000.
Proof.
  vm_compute.
  reflexivity.
Qed.

Definition tv_charge_overflow_rq : eevdf_runqueue :=
  test_add_ok eevdf_empty_runqueue 1 1 1 4000000.

Example tv_charge_overflow :
  eevdf_result_rc (eevdf_charge tv_charge_overflow_rq 1 i64_max) =
  EevdfErrOverflow.
Proof.
  vm_compute.
  reflexivity.
Qed.

Definition tv_charge_large_weight_rq : eevdf_runqueue :=
  test_add_ok eevdf_empty_runqueue 1 1 i64_max 1.

Definition tv_charge_large_weight_result : eevdf_result :=
  eevdf_charge tv_charge_large_weight_rq 1 i64_max.

Example tv_charge_large_runtime_large_weight_ok :
  eevdf_result_rc tv_charge_large_weight_result = EevdfOk.
Proof.
  vm_compute.
  reflexivity.
Qed.

Example tv_charge_large_runtime_large_weight_vruntime :
  ee_vruntime
    (nth 0
      (er_entities (eevdf_result_rq tv_charge_large_weight_result))
      eevdf_empty_entity) =
  1024.
Proof.
  vm_compute.
  reflexivity.
Qed.

Example tv_charge_large_runtime_large_weight_deadline :
  ee_deadline
    (nth 0
      (er_entities (eevdf_result_rq tv_charge_large_weight_result))
      eevdf_empty_entity) =
  1025.
Proof.
  vm_compute.
  reflexivity.
Qed.
