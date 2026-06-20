# Scheduling VST Proofs

This directory is the VST proof home for `verified/scheduling/src`.

Current layer:

- `SchedRuntimeVstSpec.v` imports VST and defines the C-facing scalar bounds,
  enum encodings, model shape predicates, representation placeholders, and
  logical postconditions for the scheduler runtime API.

Next steps:

- generate or maintain the Clight AST for `src/pacha_eevdf.c` and
  `src/pacha_sched.c`
- replace the representation placeholders with concrete `data_at`-based
  predicates for `pacha_eevdf_runqueue`, `pacha_sched_state`, and result structs
- turn the logical postconditions into VST `funspec`s
- prove the leaf helpers first, then public transitions

Build:

```sh
cd verified/scheduling/proof
nix develop /home/kamer/os --command coqc \
  -Q ../spec Pacha.Scheduling \
  -Q . Pacha.Scheduling.Proof \
  SchedRuntimeVstSpec.v
```
