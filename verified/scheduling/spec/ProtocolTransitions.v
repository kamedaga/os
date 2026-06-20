From Stdlib Require Import Arith.PeanoNat Bool.Bool.
From Pacha.Scheduling Require Import ProtocolModel.

Definition commit
    (s : system_state)
    (cpu : cpu_id)
    (tid : thread_id)
    (gen : generation)
  : option system_state :=
  if Nat.ltb cpu (cpu_count s) then
    if negb (cpu_has_owner s cpu) then
      match lookup_thread s tid with
      | Some t =>
          if generation_matches t gen then
            match ts_status t with
            | Runnable =>
                Some (replace_thread s tid (set_status t (Pending cpu)))
            | _ => None
            end
          else None
      | None => None
      end
    else None
  else None.

Definition claim
    (s : system_state)
    (cpu : cpu_id)
    (tid : thread_id)
    (gen : generation)
  : option system_state :=
  match lookup_thread s tid with
  | Some t =>
      if generation_matches t gen then
        match ts_status t with
        | Pending pending_cpu =>
            if Nat.eqb pending_cpu cpu then
              Some (replace_thread s tid (set_status t (Running cpu)))
            else None
        | _ => None
        end
      else None
  | None => None
  end.

Definition preempt
    (s : system_state)
    (cpu : cpu_id)
    (tid : thread_id)
    (gen : generation)
  : option system_state :=
  match lookup_thread s tid with
  | Some t =>
      if generation_matches t gen then
        match ts_status t with
        | Running running_cpu =>
            if Nat.eqb running_cpu cpu then
              Some (replace_thread s tid (set_status t Runnable))
            else None
        | _ => None
        end
      else None
  | None => None
  end.

Definition block
    (s : system_state)
    (cpu : cpu_id)
    (tid : thread_id)
    (gen : generation)
  : option system_state :=
  match lookup_thread s tid with
  | Some t =>
      if generation_matches t gen then
        match ts_status t with
        | Running running_cpu =>
            if Nat.eqb running_cpu cpu then
              Some (replace_thread s tid (set_status t Blocked))
            else None
        | _ => None
        end
      else None
  | None => None
  end.

Definition wake
    (s : system_state)
    (tid : thread_id)
    (gen : generation)
  : option system_state :=
  match lookup_thread s tid with
  | Some t =>
      if generation_matches t gen then
        match ts_status t with
        | Blocked =>
            Some (replace_thread s tid (set_status t Runnable))
        | _ => None
        end
      else None
  | None => None
  end.

Definition exit_thread
    (s : system_state)
    (tid : thread_id)
    (gen : generation)
  : option system_state :=
  match lookup_thread s tid with
  | Some t =>
      if generation_matches t gen then
        if is_live_status (ts_status t) then
          Some (replace_thread s tid (set_status t Exited))
        else None
      else None
  | None => None
  end.
