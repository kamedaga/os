From Stdlib Require Import Lists.List ZArith.ZArith.
From Pacha.Scheduling Require Import
  EevdfModel
  EevdfTransitions
  KernelSchedModel.

Open Scope Z_scope.

Definition ks_tv_empty_2 : kernel_sched_state :=
  kernel_sched_empty_state 2.

Example ks_tv_empty_2_cpu_count :
  ks_cpu_count ks_tv_empty_2 = 2%nat.
Proof.
  vm_compute.
  reflexivity.
Qed.

Definition ks_tv_add_cpu1_20 : kernel_sched_state * kernel_sched_result :=
  kernel_add_thread ks_tv_empty_2 1%nat 20 1 1024 4000000.

Definition ks_tv_add_cpu1_20_state : kernel_sched_state :=
  fst ks_tv_add_cpu1_20.

Example ks_tv_add_cpu1_20_ok :
  ksr_rc (snd ks_tv_add_cpu1_20) = KernelSchedOk.
Proof.
  vm_compute.
  reflexivity.
Qed.

Definition ks_tv_add_cpu1_10 : kernel_sched_state * kernel_sched_result :=
  kernel_add_thread ks_tv_add_cpu1_20_state 1%nat 10 1 1024 4000000.

Definition ks_tv_add_cpu1_10_state : kernel_sched_state :=
  fst ks_tv_add_cpu1_10.

Example ks_tv_pick_cpu0_idle :
  ksd_kind (ksr_decision (snd (kernel_pick_cpu ks_tv_add_cpu1_10_state 0%nat))) =
  KernelDecisionIdle.
Proof.
  vm_compute.
  reflexivity.
Qed.

Definition ks_tv_pick_cpu1 : kernel_sched_state * kernel_sched_result :=
  kernel_pick_cpu ks_tv_add_cpu1_10_state 1%nat.

Example ks_tv_pick_cpu1_runs_lower_thread_id :
  ksd_thread_id (ksr_decision (snd ks_tv_pick_cpu1)) = 10.
Proof.
  vm_compute.
  reflexivity.
Qed.

Example ks_tv_pick_cpu1_sets_current :
  kc_current_thread_id
    (nth 1%nat
      (ks_cpus (fst ks_tv_pick_cpu1))
      kernel_empty_cpu) =
  10.
Proof.
  vm_compute.
  reflexivity.
Qed.

Definition ks_tv_timer_cpu1 : kernel_sched_state * kernel_sched_result :=
  kernel_on_timer (fst ks_tv_pick_cpu1) 1%nat 1000.

Example ks_tv_timer_cpu1_keeps_current_thread :
  kc_current_thread_id
    (nth 1%nat
      (ks_cpus (fst ks_tv_timer_cpu1))
      kernel_empty_cpu) =
  10.
Proof.
  vm_compute.
  reflexivity.
Qed.

Example ks_tv_timer_cpu1_keeps_current_generation :
  kc_current_generation
    (nth 1%nat
      (ks_cpus (fst ks_tv_timer_cpu1))
      kernel_empty_cpu) =
  1.
Proof.
  vm_compute.
  reflexivity.
Qed.

Example ks_tv_timer_cpu1_charges_service :
  let rq :=
    nth 1%nat
      (ks_runqueues (fst ks_tv_timer_cpu1))
      eevdf_empty_runqueue in
  match find_entity_index rq 10 with
  | Some index =>
      ee_service_ns (nth index (er_entities rq) eevdf_empty_entity)
  | None => -1
  end =
  1000.
Proof.
  vm_compute.
  reflexivity.
Qed.

Definition ks_tv_finish_cpu1 : kernel_sched_state * kernel_sched_result :=
  kernel_finish_current (fst ks_tv_timer_cpu1) 1%nat.

Example ks_tv_finish_cpu1_clears_current :
  kc_has_current
    (nth 1%nat
      (ks_cpus (fst ks_tv_finish_cpu1))
      kernel_empty_cpu) =
  false.
Proof.
  vm_compute.
  reflexivity.
Qed.

Example ks_tv_finish_cpu1_requeues_thread :
  er_runnable_count
    (nth 1%nat
      (ks_runqueues (fst ks_tv_finish_cpu1))
      eevdf_empty_runqueue) =
  2%nat.
Proof.
  vm_compute.
  reflexivity.
Qed.

Definition ks_tv_duplicate_base : kernel_sched_state :=
  fst (kernel_add_thread ks_tv_empty_2 0%nat 9 1 1024 4000000).

Example ks_tv_duplicate_rejected_across_cpus :
  ksr_rc
    (snd (kernel_add_thread ks_tv_duplicate_base 1%nat 9 2 1024 4000000)) =
  KernelSchedErrInvalid.
Proof.
  vm_compute.
  reflexivity.
Qed.

Definition ks_tv_activation_requested : kernel_sched_state :=
  fst (kernel_request_activation (kernel_sched_empty_state 1) 0%nat).

Example ks_tv_activation_pending_set :
  kc_activation_pending
    (nth 0%nat (ks_cpus ks_tv_activation_requested) kernel_empty_cpu) =
  true.
Proof.
  vm_compute.
  reflexivity.
Qed.

Definition ks_tv_activation_added : kernel_sched_state :=
  fst (kernel_add_thread ks_tv_activation_requested 0%nat 7 2 1024 4000000).

Definition ks_tv_activation_claimed : kernel_sched_state * kernel_sched_result :=
  kernel_claim_activation ks_tv_activation_added 0%nat.

Example ks_tv_activation_claim_runs_thread :
  ksd_thread_id (ksr_decision (snd ks_tv_activation_claimed)) = 7.
Proof.
  vm_compute.
  reflexivity.
Qed.

Example ks_tv_activation_claim_clears_pending :
  kc_activation_pending
    (nth 0%nat (ks_cpus (fst ks_tv_activation_claimed)) kernel_empty_cpu) =
  false.
Proof.
  vm_compute.
  reflexivity.
Qed.

Definition ks_tv_handoff_base : kernel_sched_state :=
  fst (kernel_add_thread
    (fst (kernel_add_thread (kernel_sched_empty_state 1) 0%nat 10 1 1024 4000000))
    0%nat 20 1 1024 4000000).

Definition ks_tv_handoff_picked : kernel_sched_state :=
  fst (kernel_pick_cpu ks_tv_handoff_base 0%nat).

Definition ks_tv_handoff_result : kernel_sched_state * kernel_sched_result :=
  kernel_handoff_to_thread_on_cpu ks_tv_handoff_picked 0%nat 20.

Example ks_tv_handoff_runs_target :
  ksd_thread_id (ksr_decision (snd ks_tv_handoff_result)) = 20.
Proof.
  vm_compute.
  reflexivity.
Qed.

Example ks_tv_handoff_sets_current :
  kc_current_thread_id
    (nth 0%nat
      (ks_cpus (fst ks_tv_handoff_result))
      kernel_empty_cpu) =
  20.
Proof.
  vm_compute.
  reflexivity.
Qed.

Example ks_tv_handoff_keeps_one_runnable :
  er_runnable_count
    (nth 0%nat
      (ks_runqueues (fst ks_tv_handoff_result))
      eevdf_empty_runqueue) =
  1%nat.
Proof.
  vm_compute.
  reflexivity.
Qed.

Definition ks_tv_thread30_base : kernel_sched_state :=
  fst (kernel_add_thread ks_tv_empty_2 1%nat 30 1 1024 4000000).

Definition ks_tv_thread30_picked : kernel_sched_state :=
  fst (kernel_pick_cpu ks_tv_thread30_base 1%nat).

Definition ks_tv_thread30_blocked : kernel_sched_state * kernel_sched_result :=
  kernel_block_thread ks_tv_thread30_picked 30.

Example ks_tv_block_clears_current :
  kc_has_current
    (nth 1%nat
      (ks_cpus (fst ks_tv_thread30_blocked))
      kernel_empty_cpu) =
  false.
Proof.
  vm_compute.
  reflexivity.
Qed.

Example ks_tv_block_removes_runnable :
  er_runnable_count
    (nth 1%nat
      (ks_runqueues (fst ks_tv_thread30_blocked))
      eevdf_empty_runqueue) =
  0%nat.
Proof.
  vm_compute.
  reflexivity.
Qed.

Definition ks_tv_thread30_woken : kernel_sched_state * kernel_sched_result :=
  kernel_wake_thread (fst ks_tv_thread30_blocked) 30.

Example ks_tv_wake_restores_runnable :
  er_runnable_count
    (nth 1%nat
      (ks_runqueues (fst ks_tv_thread30_woken))
      eevdf_empty_runqueue) =
  1%nat.
Proof.
  vm_compute.
  reflexivity.
Qed.

Definition ks_tv_thread30_exited : kernel_sched_state * kernel_sched_result :=
  kernel_exit_thread (fst ks_tv_thread30_woken) 30.

Example ks_tv_exit_removes_runnable :
  er_runnable_count
    (nth 1%nat
      (ks_runqueues (fst ks_tv_thread30_exited))
      eevdf_empty_runqueue) =
  0%nat.
Proof.
  vm_compute.
  reflexivity.
Qed.

Definition ks_tv_stale_current : kernel_sched_state :=
  kernel_set_cpu_current (fst ks_tv_thread30_exited) 1%nat 30 1.

Definition ks_tv_stale_timer : kernel_sched_state * kernel_sched_result :=
  kernel_on_timer ks_tv_stale_current 1%nat 1000.

Example ks_tv_stale_timer_ok :
  ksr_rc (snd ks_tv_stale_timer) = KernelSchedOk.
Proof.
  vm_compute.
  reflexivity.
Qed.

Example ks_tv_stale_timer_clears_current :
  kc_has_current
    (nth 1%nat
      (ks_cpus (fst ks_tv_stale_timer))
      kernel_empty_cpu) =
  false.
Proof.
  vm_compute.
  reflexivity.
Qed.

Example ks_tv_stale_timer_does_not_find_exited :
  let rq :=
    nth 1%nat
      (ks_runqueues (fst ks_tv_stale_timer))
      eevdf_empty_runqueue in
  find_entity_index rq 30 = None.
Proof.
  vm_compute.
  reflexivity.
Qed.

Definition ks_tv_stale_finish : kernel_sched_state * kernel_sched_result :=
  kernel_finish_current ks_tv_stale_current 1%nat.

Example ks_tv_stale_finish_ok :
  ksr_rc (snd ks_tv_stale_finish) = KernelSchedOk.
Proof.
  vm_compute.
  reflexivity.
Qed.

Example ks_tv_stale_finish_clears_current :
  kc_has_current
    (nth 1%nat
      (ks_cpus (fst ks_tv_stale_finish))
      kernel_empty_cpu) =
  false.
Proof.
  vm_compute.
  reflexivity.
Qed.

Definition ks_tv_migrate_base : kernel_sched_state :=
  fst (kernel_add_thread ks_tv_empty_2 0%nat 11 1 1024 4000000).

Example ks_tv_migrate_same_cpu_rejected :
  ksr_rc (snd (kernel_migrate_runnable ks_tv_migrate_base 0%nat 0%nat 11)) =
  KernelSchedErrInvalid.
Proof.
  vm_compute.
  reflexivity.
Qed.

Definition ks_tv_migrated_to_cpu1 : kernel_sched_state * kernel_sched_result :=
  kernel_migrate_runnable ks_tv_migrate_base 0%nat 1%nat 11.

Example ks_tv_migrate_to_cpu1_ok :
  ksr_rc (snd ks_tv_migrated_to_cpu1) = KernelSchedOk.
Proof.
  vm_compute.
  reflexivity.
Qed.

Example ks_tv_migrate_source_empty :
  er_entity_count
    (nth 0%nat
      (ks_runqueues (fst ks_tv_migrated_to_cpu1))
      eevdf_empty_runqueue) =
  0%nat.
Proof.
  vm_compute.
  reflexivity.
Qed.

Example ks_tv_migrate_destination_runnable :
  er_runnable_count
    (nth 1%nat
      (ks_runqueues (fst ks_tv_migrated_to_cpu1))
      eevdf_empty_runqueue) =
  1%nat.
Proof.
  vm_compute.
  reflexivity.
Qed.

Definition ks_tv_migrate_floor_src : kernel_sched_state :=
  fst (kernel_add_thread ks_tv_empty_2 0%nat 11 1 1024 4000000).

Definition ks_tv_migrate_floor_added_dst : kernel_sched_state :=
  fst (kernel_add_thread ks_tv_migrate_floor_src 1%nat 22 2 1024 4000000).

Definition ks_tv_migrate_floor_picked_dst : kernel_sched_state :=
  fst (kernel_pick_cpu ks_tv_migrate_floor_added_dst 1%nat).

Definition ks_tv_migrate_floor_timer_dst : kernel_sched_state :=
  fst (kernel_on_timer ks_tv_migrate_floor_picked_dst 1%nat 4096).

Definition ks_tv_migrate_floor_ready : kernel_sched_state :=
  fst (kernel_finish_current ks_tv_migrate_floor_timer_dst 1%nat).

Definition ks_tv_migrate_floor_before : Z :=
  er_min_vruntime
    (nth 1%nat
      (ks_runqueues ks_tv_migrate_floor_ready)
      eevdf_empty_runqueue).

Example ks_tv_migrate_floor_before_positive :
  ks_tv_migrate_floor_before = 4096.
Proof.
  vm_compute.
  reflexivity.
Qed.

Definition ks_tv_migrated_to_floor_cpu1 : kernel_sched_state * kernel_sched_result :=
  kernel_migrate_runnable ks_tv_migrate_floor_ready 0%nat 1%nat 11.

Example ks_tv_migrate_floor_ok :
  ksr_rc (snd ks_tv_migrated_to_floor_cpu1) = KernelSchedOk.
Proof.
  vm_compute.
  reflexivity.
Qed.

Example ks_tv_migrate_floor_destination_count :
  er_entity_count
    (nth 1%nat
      (ks_runqueues (fst ks_tv_migrated_to_floor_cpu1))
      eevdf_empty_runqueue) =
  2%nat.
Proof.
  vm_compute.
  reflexivity.
Qed.

Example ks_tv_migrate_floor_moved_vruntime :
  let rq :=
    nth 1%nat
      (ks_runqueues (fst ks_tv_migrated_to_floor_cpu1))
      eevdf_empty_runqueue in
  match find_entity_index rq 11 with
  | Some index =>
      ee_vruntime (nth index (er_entities rq) eevdf_empty_entity)
  | None => -1
  end =
  ks_tv_migrate_floor_before.
Proof.
  vm_compute.
  reflexivity.
Qed.

Example ks_tv_migrate_floor_moved_generation :
  let rq :=
    nth 1%nat
      (ks_runqueues (fst ks_tv_migrated_to_floor_cpu1))
      eevdf_empty_runqueue in
  match find_entity_index rq 11 with
  | Some index =>
      ee_generation (nth index (er_entities rq) eevdf_empty_entity)
  | None => -1
  end =
  1.
Proof.
  vm_compute.
  reflexivity.
Qed.

Definition ks_tv_migrate_current_base : kernel_sched_state :=
  fst (kernel_add_thread ks_tv_empty_2 0%nat 33 1 1024 4000000).

Definition ks_tv_migrate_current_dst_added : kernel_sched_state :=
  fst (kernel_add_thread ks_tv_migrate_current_base 1%nat 22 1 1024 4000000).

Definition ks_tv_migrate_current_src_picked : kernel_sched_state :=
  fst (kernel_pick_cpu ks_tv_migrate_current_dst_added 0%nat).

Definition ks_tv_migrate_current_src_added : kernel_sched_state :=
  fst (kernel_add_thread ks_tv_migrate_current_src_picked 0%nat 11 1 1024 4000000).

Definition ks_tv_migrate_current_dst_picked : kernel_sched_state :=
  fst (kernel_pick_cpu ks_tv_migrate_current_src_added 1%nat).

Definition ks_tv_migrate_current_done : kernel_sched_state * kernel_sched_result :=
  kernel_migrate_runnable ks_tv_migrate_current_dst_picked 0%nat 1%nat 11.

Example ks_tv_migrate_current_ok :
  ksr_rc (snd ks_tv_migrate_current_done) = KernelSchedOk.
Proof.
  vm_compute.
  reflexivity.
Qed.

Example ks_tv_migrate_current_keeps_src_current :
  kc_current_thread_id
    (nth 0%nat
      (ks_cpus (fst ks_tv_migrate_current_done))
      kernel_empty_cpu) =
  33.
Proof.
  vm_compute.
  reflexivity.
Qed.

Example ks_tv_migrate_current_keeps_dst_current :
  kc_current_thread_id
    (nth 1%nat
      (ks_cpus (fst ks_tv_migrate_current_done))
      kernel_empty_cpu) =
  22.
Proof.
  vm_compute.
  reflexivity.
Qed.

Example ks_tv_migrate_current_moved_is_runnable :
  let rq :=
    nth 1%nat
      (ks_runqueues (fst ks_tv_migrate_current_done))
      eevdf_empty_runqueue in
  match find_entity_index rq 11 with
  | Some index =>
      ee_state (nth index (er_entities rq) eevdf_empty_entity)
  | None => EEmpty
  end =
  ERunnable.
Proof.
  vm_compute.
  reflexivity.
Qed.
