# Scheduling VST Proofs

This directory is the VST proof home for `verified/scheduling/src`.

Current layer:

- `SchedRuntimeVstSpec.v` imports VST and defines the C-facing scalar bounds,
  enum encodings, model shape predicates, representation placeholders, and
  logical postconditions for the scheduler runtime API.
- `generated/` contains Clight ASTs produced from the canonical C sources.
- `PachaSchedVst.v` builds `CompSpecs` for the scheduler translation unit and
  defines `data_at` representations for scheduler structs, plus small
  `field_at` views for leaf structs where helper proofs will start.

Next steps:

- generate or maintain the Clight AST for `src/pacha_eevdf.c` and
  `src/pacha_sched.c`
- turn the logical postconditions into VST `funspec`s
- prove the leaf helpers first, then public transitions

ABI note:

- VST-facing C functions use explicit `pacha_*_rc` returns and out-parameters.
  `clightgen` therefore runs without `-fstruct-passing`.

Build:

```sh
cd verified/scheduling/proof
nix develop /home/kamer/os --command ./gen-clight.sh
nix develop /home/kamer/os --command ./verify-coq.sh
```

`verify-coq.sh` builds from `verified/scheduling` so `spec/` and `proof/`
share one dependency graph. It uses `coq_makefile` and `make -j$(nproc)` by
default, so it can use all local CPU threads. Override with `COQ_JOBS=12` when
you want to leave cores free:

```sh
nix develop /home/kamer/os --command env COQ_JOBS=12 ./verify-coq.sh
```

For a fully clean proof rebuild:

```sh
nix develop /home/kamer/os --command ./verify-coq.sh clean
nix develop /home/kamer/os --command ./verify-coq.sh
```

To iterate on one proof file:

```sh
nix develop /home/kamer/os --command ./verify-coq.sh build proof/PachaEevdfVst.vo
```
