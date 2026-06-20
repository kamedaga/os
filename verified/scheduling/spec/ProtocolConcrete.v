From Stdlib Require Import Arith.PeanoNat Bool.Bool Lists.List.
From Pacha.Scheduling Require Import ProtocolModel.

Import ListNotations.

Definition max_cpus : nat := 256.
Definition max_threads : nat := 4096.
Definition invalid_thread_id : nat := max_threads.

Inductive sched_rc : Type :=
| SchedOk
| SchedErrBounds
| SchedErrGeneration
| SchedErrState
| SchedErrBusy.

Inductive concrete_status : Type :=
| CEmpty
| CRunnable
| CRunning
| CPending
| CBlocked
| CExited.

Record concrete_thread : Type := {
  ct_generation : generation;
  ct_status : concrete_status;
  ct_cpu : cpu_id;
}.

Record concrete_cpu : Type := {
  cc_current_tid : thread_id;
  cc_current_generation : generation;
}.

Record concrete_state : Type := {
  cs_cpu_count : nat;
  cs_thread_count : nat;
  cs_threads : list concrete_thread;
  cs_cpus : list concrete_cpu;
}.

Record sched_result : Type := {
  sr_rc : sched_rc;
  sr_state : concrete_state;
}.

Definition c_status_owner_cpu
    (thread : concrete_thread)
  : option cpu_id :=
  match ct_status thread with
  | CRunning => Some (ct_cpu thread)
  | CPending => Some (ct_cpu thread)
  | _ => None
  end.

Definition c_status_owns_cpu
    (thread : concrete_thread)
    (cpu : cpu_id)
  : bool :=
  match c_status_owner_cpu thread with
  | Some owner => Nat.eqb owner cpu
  | None => false
  end.

Definition c_is_live_status
    (status : concrete_status)
  : bool :=
  match status with
  | CEmpty => false
  | CExited => false
  | _ => true
  end.

Definition concrete_thread_to_abstract
    (thread : concrete_thread)
  : thread_state :=
  {|
    ts_generation := ct_generation thread;
    ts_status :=
      match ct_status thread with
      | CEmpty => Empty
      | CRunnable => Runnable
      | CRunning => Running (ct_cpu thread)
      | CPending => Pending (ct_cpu thread)
      | CBlocked => Blocked
      | CExited => Exited
      end;
  |}.

Definition concrete_lookup_thread_raw
    (s : concrete_state)
    (tid : thread_id)
  : option concrete_thread :=
  nth_error (cs_threads s) tid.

Definition concrete_lookup_cpu_raw
    (s : concrete_state)
    (cpu : cpu_id)
  : option concrete_cpu :=
  nth_error (cs_cpus s) cpu.

Definition concrete_valid_cpu
    (s : concrete_state)
    (cpu : cpu_id)
  : bool :=
  Nat.ltb cpu (cs_cpu_count s).

Definition concrete_valid_thread
    (s : concrete_state)
    (tid : thread_id)
  : bool :=
  Nat.ltb tid (cs_thread_count s).

Definition concrete_lookup_thread
    (s : concrete_state)
    (tid : thread_id)
  : option concrete_thread :=
  if concrete_valid_thread s tid then
    concrete_lookup_thread_raw s tid
  else None.

Definition concrete_cpu_has_owner
    (s : concrete_state)
    (cpu : cpu_id)
  : bool :=
  existsb (fun thread => c_status_owns_cpu thread cpu) (cs_threads s).

Definition concrete_set_status
    (thread : concrete_thread)
    (status : concrete_status)
    (cpu : cpu_id)
  : concrete_thread :=
  {|
    ct_generation := ct_generation thread;
    ct_status := status;
    ct_cpu := cpu;
  |}.

Definition replace_concrete_thread
    (s : concrete_state)
    (tid : thread_id)
    (thread : concrete_thread)
  : concrete_state :=
  {|
    cs_cpu_count := cs_cpu_count s;
    cs_thread_count := cs_thread_count s;
    cs_threads := replace_nth (cs_threads s) tid thread;
    cs_cpus := cs_cpus s;
  |}.

Fixpoint running_thread_for_from
    (remaining : nat)
    (threads : list concrete_thread)
    (cpu : cpu_id)
    (tid : thread_id)
  : option (thread_id * generation) :=
  match remaining, threads with
  | O, _ => None
  | _, [] => None
  | S remaining', thread :: rest =>
      if andb
          match ct_status thread with
          | CRunning => true
          | _ => false
          end
          (Nat.eqb (ct_cpu thread) cpu)
      then Some (tid, ct_generation thread)
      else running_thread_for_from remaining' rest cpu (S tid)
  end.

Definition running_thread_for
    (s : concrete_state)
    (cpu : cpu_id)
  : option (thread_id * generation) :=
  if concrete_valid_cpu s cpu then
    running_thread_for_from (cs_thread_count s) (cs_threads s) cpu 0
  else None.

Definition concrete_cpu_for
    (s : concrete_state)
    (cpu : cpu_id)
  : concrete_cpu :=
  match running_thread_for s cpu with
  | Some (tid, gen) =>
      {|
        cc_current_tid := tid;
        cc_current_generation := gen;
      |}
  | None =>
      {|
        cc_current_tid := invalid_thread_id;
        cc_current_generation := 0;
      |}
  end.

Definition computed_cpus
    (s : concrete_state)
  : list concrete_cpu :=
  map (concrete_cpu_for s) (seq 0 max_cpus).

Definition sync_cpu_table
    (s : concrete_state)
  : concrete_state :=
  {|
    cs_cpu_count := cs_cpu_count s;
    cs_thread_count := cs_thread_count s;
    cs_threads := cs_threads s;
    cs_cpus := computed_cpus s;
  |}.

Definition ok
    (s : concrete_state)
  : sched_result :=
  {|
    sr_rc := SchedOk;
    sr_state := s;
  |}.

Definition fail
    (rc : sched_rc)
    (s : concrete_state)
  : sched_result :=
  {|
    sr_rc := rc;
    sr_state := s;
  |}.

Definition sched_commit
    (s : concrete_state)
    (cpu : cpu_id)
    (tid : thread_id)
    (gen : generation)
  : sched_result :=
  if andb (concrete_valid_cpu s cpu) (concrete_valid_thread s tid) then
    if negb (concrete_cpu_has_owner s cpu) then
      match concrete_lookup_thread s tid with
      | Some thread =>
          if Nat.eqb (ct_generation thread) gen then
            match ct_status thread with
            | CRunnable =>
                ok (sync_cpu_table (replace_concrete_thread s tid
                  (concrete_set_status thread CPending cpu)))
            | _ => fail SchedErrState s
            end
          else fail SchedErrGeneration s
      | None => fail SchedErrBounds s
      end
    else fail SchedErrBusy s
  else fail SchedErrBounds s.

Definition sched_claim
    (s : concrete_state)
    (cpu : cpu_id)
    (tid : thread_id)
    (gen : generation)
  : sched_result :=
  if andb (concrete_valid_cpu s cpu) (concrete_valid_thread s tid) then
    match concrete_lookup_thread s tid with
    | Some thread =>
        if Nat.eqb (ct_generation thread) gen then
          match ct_status thread with
          | CPending =>
              if Nat.eqb (ct_cpu thread) cpu then
                ok (sync_cpu_table (replace_concrete_thread s tid
                  (concrete_set_status thread CRunning cpu)))
              else fail SchedErrState s
          | _ => fail SchedErrState s
          end
        else fail SchedErrGeneration s
    | None => fail SchedErrBounds s
    end
  else fail SchedErrBounds s.

Definition sched_preempt
    (s : concrete_state)
    (cpu : cpu_id)
    (tid : thread_id)
    (gen : generation)
  : sched_result :=
  if andb (concrete_valid_cpu s cpu) (concrete_valid_thread s tid) then
    match concrete_lookup_thread s tid with
    | Some thread =>
        if Nat.eqb (ct_generation thread) gen then
          match ct_status thread with
          | CRunning =>
              if Nat.eqb (ct_cpu thread) cpu then
                ok (sync_cpu_table (replace_concrete_thread s tid
                  (concrete_set_status thread CRunnable cpu)))
              else fail SchedErrState s
          | _ => fail SchedErrState s
          end
        else fail SchedErrGeneration s
    | None => fail SchedErrBounds s
    end
  else fail SchedErrBounds s.

Definition sched_block
    (s : concrete_state)
    (cpu : cpu_id)
    (tid : thread_id)
    (gen : generation)
  : sched_result :=
  if andb (concrete_valid_cpu s cpu) (concrete_valid_thread s tid) then
    match concrete_lookup_thread s tid with
    | Some thread =>
        if Nat.eqb (ct_generation thread) gen then
          match ct_status thread with
          | CRunning =>
              if Nat.eqb (ct_cpu thread) cpu then
                ok (sync_cpu_table (replace_concrete_thread s tid
                  (concrete_set_status thread CBlocked cpu)))
              else fail SchedErrState s
          | _ => fail SchedErrState s
          end
        else fail SchedErrGeneration s
    | None => fail SchedErrBounds s
    end
  else fail SchedErrBounds s.

Definition sched_wake
    (s : concrete_state)
    (tid : thread_id)
    (gen : generation)
  : sched_result :=
  if concrete_valid_thread s tid then
    match concrete_lookup_thread s tid with
    | Some thread =>
        if Nat.eqb (ct_generation thread) gen then
          match ct_status thread with
          | CBlocked =>
              ok (sync_cpu_table (replace_concrete_thread s tid
                (concrete_set_status thread CRunnable (ct_cpu thread))))
          | _ => fail SchedErrState s
          end
        else fail SchedErrGeneration s
    | None => fail SchedErrBounds s
    end
  else fail SchedErrBounds s.

Definition sched_exit_thread
    (s : concrete_state)
    (tid : thread_id)
    (gen : generation)
  : sched_result :=
  if concrete_valid_thread s tid then
    match concrete_lookup_thread s tid with
    | Some thread =>
        if Nat.eqb (ct_generation thread) gen then
          if c_is_live_status (ct_status thread) then
            ok (sync_cpu_table (replace_concrete_thread s tid
              (concrete_set_status thread CExited (ct_cpu thread))))
          else fail SchedErrState s
        else fail SchedErrGeneration s
    | None => fail SchedErrBounds s
    end
  else fail SchedErrBounds s.
