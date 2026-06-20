From Stdlib Require Import Bool.Bool Lia Lists.List ZArith.ZArith.
From Pacha.Scheduling Require Import EevdfModel EevdfInvariants.

Open Scope Z_scope.

Theorem weighted_delta_weight_zero_fails :
  forall runtime_ns,
    weighted_delta runtime_ns 0 = None.
Proof.
  intros runtime_ns.
  unfold weighted_delta, valid_positive.
  simpl.
  destruct (i64_nonnegative runtime_ns); reflexivity.
Qed.

Theorem eevdf_charge_success_formula :
  forall rq thread_id runtime_ns rq',
    eevdf_charge rq thread_id runtime_ns =
      {|
        eevdf_result_rc := EevdfOk;
        eevdf_result_rq := rq';
      |} ->
    exists index old_entity charged_entity refreshed delta,
      find_entity_index rq thread_id = Some index /\
      lookup_entity rq index = Some old_entity /\
      runnable_or_running old_entity = true /\
      weighted_delta runtime_ns (ee_weight old_entity) = Some delta /\
      i64_value (ee_vruntime old_entity + delta) = true /\
      ee_thread_id charged_entity = ee_thread_id old_entity /\
      ee_weight charged_entity = ee_weight old_entity /\
      ee_slice_ns charged_entity = ee_slice_ns old_entity /\
      ee_state charged_entity = ee_state old_entity /\
      ee_vruntime charged_entity = ee_vruntime old_entity + delta /\
      ee_service_ns charged_entity = ee_service_ns old_entity + runtime_ns /\
      refresh_deadline charged_entity (er_min_vruntime rq) = Some refreshed /\
      rq' = refresh_runqueue (replace_entity rq index refreshed).
Proof.
  intros rq thread_id runtime_ns rq' Hcharge.
  unfold eevdf_charge in Hcharge.
  destruct (find_entity_index rq thread_id) as [index |] eqn:Hfind;
    try discriminate.
  destruct (lookup_entity rq index) as [old_entity |] eqn:Hlookup;
    try discriminate.
  destruct old_entity as
    [old_thread_id old_generation old_weight old_slice_ns old_service_ns
      old_vruntime old_eligible old_deadline old_state].
  simpl in *.
  destruct old_state; try discriminate.
  - destruct (weighted_delta runtime_ns old_weight) as [delta |] eqn:Hdelta;
      try discriminate.
    destruct (i64_value (old_vruntime + delta)) eqn:Hrange;
      try discriminate.
    destruct (refresh_deadline
      {|
        ee_thread_id := old_thread_id;
        ee_generation := old_generation;
        ee_weight := old_weight;
        ee_slice_ns := old_slice_ns;
        ee_service_ns := old_service_ns + runtime_ns;
        ee_vruntime := old_vruntime + delta;
        ee_eligible_time := old_eligible;
        ee_deadline := old_deadline;
        ee_state := ERunnable;
      |}
      (er_min_vruntime rq)) as [refreshed |] eqn:Hrefresh;
      try discriminate.
    inversion Hcharge; subst.
    exists index.
    exists {|
      ee_thread_id := old_thread_id;
      ee_generation := old_generation;
      ee_weight := old_weight;
      ee_slice_ns := old_slice_ns;
      ee_service_ns := old_service_ns;
      ee_vruntime := old_vruntime;
      ee_eligible_time := old_eligible;
      ee_deadline := old_deadline;
      ee_state := ERunnable;
    |}.
    exists {|
      ee_thread_id := old_thread_id;
      ee_generation := old_generation;
      ee_weight := old_weight;
      ee_slice_ns := old_slice_ns;
      ee_service_ns := old_service_ns + runtime_ns;
      ee_vruntime := old_vruntime + delta;
      ee_eligible_time := old_eligible;
      ee_deadline := old_deadline;
      ee_state := ERunnable;
    |}.
    exists refreshed.
    exists delta.
    repeat split; simpl; auto; try reflexivity.
  - destruct (weighted_delta runtime_ns old_weight) as [delta |] eqn:Hdelta;
      try discriminate.
    destruct (i64_value (old_vruntime + delta)) eqn:Hrange;
      try discriminate.
    destruct (refresh_deadline
      {|
        ee_thread_id := old_thread_id;
        ee_generation := old_generation;
        ee_weight := old_weight;
        ee_slice_ns := old_slice_ns;
        ee_service_ns := old_service_ns + runtime_ns;
        ee_vruntime := old_vruntime + delta;
        ee_eligible_time := old_eligible;
        ee_deadline := old_deadline;
        ee_state := ERunning;
      |}
      (er_min_vruntime rq)) as [refreshed |] eqn:Hrefresh;
      try discriminate.
    inversion Hcharge; subst.
    exists index.
    exists {|
      ee_thread_id := old_thread_id;
      ee_generation := old_generation;
      ee_weight := old_weight;
      ee_slice_ns := old_slice_ns;
      ee_service_ns := old_service_ns;
      ee_vruntime := old_vruntime;
      ee_eligible_time := old_eligible;
      ee_deadline := old_deadline;
      ee_state := ERunning;
    |}.
    exists {|
      ee_thread_id := old_thread_id;
      ee_generation := old_generation;
      ee_weight := old_weight;
      ee_slice_ns := old_slice_ns;
      ee_service_ns := old_service_ns + runtime_ns;
      ee_vruntime := old_vruntime + delta;
      ee_eligible_time := old_eligible;
      ee_deadline := old_deadline;
      ee_state := ERunning;
    |}.
    exists refreshed.
    exists delta.
    repeat split; simpl; auto; try reflexivity.
Qed.

Theorem eevdf_charge_weight_zero_fails :
  forall rq thread_id runtime_ns index entity,
    find_entity_index rq thread_id = Some index ->
    lookup_entity rq index = Some entity ->
    runnable_or_running entity = true ->
    ee_weight entity = 0 ->
    eevdf_charge rq thread_id runtime_ns = fail EevdfErrOverflow rq.
Proof.
  intros rq thread_id runtime_ns index entity Hfind Hlookup Hstate Hweight.
  unfold eevdf_charge.
  rewrite Hfind.
  rewrite Hlookup.
  destruct entity as
    [old_thread_id old_generation old_weight old_slice_ns old_service_ns
      old_vruntime old_eligible old_deadline old_state].
  simpl in *.
  subst old_weight.
  destruct old_state; try discriminate; simpl in *;
    rewrite weighted_delta_weight_zero_fails; reflexivity.
Qed.

Theorem eevdf_charge_delta_overflow_fails :
  forall rq thread_id runtime_ns index entity,
    find_entity_index rq thread_id = Some index ->
    lookup_entity rq index = Some entity ->
    runnable_or_running entity = true ->
    weighted_delta runtime_ns (ee_weight entity) = None ->
    eevdf_charge rq thread_id runtime_ns = fail EevdfErrOverflow rq.
Proof.
  intros rq thread_id runtime_ns index entity Hfind Hlookup Hstate Hdelta.
  unfold eevdf_charge.
  rewrite Hfind.
  rewrite Hlookup.
  destruct entity as
    [old_thread_id old_generation old_weight old_slice_ns old_service_ns
      old_vruntime old_eligible old_deadline old_state].
  simpl in *.
  destruct old_state; try discriminate; simpl in *; rewrite Hdelta; reflexivity.
Qed.

Theorem eevdf_charge_vruntime_overflow_fails :
  forall rq thread_id runtime_ns index entity delta,
    find_entity_index rq thread_id = Some index ->
    lookup_entity rq index = Some entity ->
    runnable_or_running entity = true ->
    weighted_delta runtime_ns (ee_weight entity) = Some delta ->
    i64_value (ee_vruntime entity + delta) = false ->
    eevdf_charge rq thread_id runtime_ns = fail EevdfErrOverflow rq.
Proof.
  intros rq thread_id runtime_ns index entity delta
    Hfind Hlookup Hstate Hdelta Hrange.
  unfold eevdf_charge.
  rewrite Hfind.
  rewrite Hlookup.
  destruct entity as
    [old_thread_id old_generation old_weight old_slice_ns old_service_ns
      old_vruntime old_eligible old_deadline old_state].
  simpl in *.
  destruct old_state; try discriminate; simpl in *;
    rewrite Hdelta; rewrite Hrange; reflexivity.
Qed.
