From Stdlib Require Import Arith.PeanoNat Bool.Bool Lists.List.
Import ListNotations.

Definition cpu_id := nat.
Definition thread_id := nat.
Definition generation := nat.

Inductive thread_status : Type :=
| Empty
| Runnable
| Running (cpu : cpu_id)
| Pending (cpu : cpu_id)
| Blocked
| Exited.

Record thread_state : Type := {
  ts_generation : generation;
  ts_status : thread_status;
}.

Record system_state : Type := {
  cpu_count : nat;
  threads : list thread_state;
}.

Definition valid_cpu (s : system_state) (cpu : cpu_id) : Prop :=
  cpu < cpu_count s.

Definition lookup_thread
    (s : system_state)
    (tid : thread_id)
  : option thread_state :=
  nth_error (threads s) tid.

Definition generation_matches
    (t : thread_state)
    (gen : generation)
  : bool :=
  Nat.eqb (ts_generation t) gen.

Definition set_status
    (t : thread_state)
    (status : thread_status)
  : thread_state :=
  {|
    ts_generation := ts_generation t;
    ts_status := status;
  |}.

Fixpoint replace_nth {A : Type}
    (items : list A)
    (index : nat)
    (item : A)
  : list A :=
  match items, index with
  | [], _ => []
  | _ :: rest, O => item :: rest
  | head :: rest, S index' => head :: replace_nth rest index' item
  end.

Definition replace_thread
    (s : system_state)
    (tid : thread_id)
    (t : thread_state)
  : system_state :=
  {|
    cpu_count := cpu_count s;
    threads := replace_nth (threads s) tid t;
  |}.

Definition status_owner_cpu (status : thread_status) : option cpu_id :=
  match status with
  | Running cpu => Some cpu
  | Pending cpu => Some cpu
  | _ => None
  end.

Definition status_owns_cpu
    (status : thread_status)
    (cpu : cpu_id)
  : bool :=
  match status_owner_cpu status with
  | Some owner => Nat.eqb owner cpu
  | None => false
  end.

Definition cpu_has_owner
    (s : system_state)
    (cpu : cpu_id)
  : bool :=
  existsb (fun t => status_owns_cpu (ts_status t) cpu) (threads s).

Definition is_live_status (status : thread_status) : bool :=
  match status with
  | Empty => false
  | Exited => false
  | _ => true
  end.
