# Verified Scheduling

This directory is the canonical implementation home for scheduling logic used
by the kernel scheduler.

The code here is intended to become the implementation and proof source for
kernel scheduling decisions. Older userland-scheduler experiments can still be
useful as tests or trace generators, but the long-term scheduler boundary is no
longer a split `schedulerd` policy daemon. The kernel owns scheduler activation,
per-CPU dispatch, and context-switch integration.

The scheduling implementation is split into two layers:

- EEVDF core: runqueue ordering, charging, eligibility, and pick rules
- Kernel scheduler core: per-CPU runqueues, CPU current-thread bookkeeping,
  activation, preempt/block/wake state transitions, and migration policy

The verified implementation must not know about:

- kernel trap frames
- CR3, PKRU, FS/GS MSRs
- file descriptors or syscalls
- serial output or logging
- allocation strategies

These remain outside the verified scheduling core:

- trap frame save/restore
- CR3, PKRU, FS/GS MSRs
- AP startup, IPIs, and interrupt plumbing
- fd/event queue exposure
- actual context switch execution

The implementation should expose pure or almost-pure transition functions. A
typical shape is:

```text
state + event -> state + decision
```

Important invariants:

- a thread generation has exactly one ownership state
- a thread generation is running on at most one CPU
- a pending commit and a running CPU do not own the same thread at once
- blocked and exited threads are never picked
- generation mismatches are rejected
- a CPU can claim only a thread pending for that CPU
- EEVDF pick chooses an eligible entity with the smallest deadline
- per-CPU EEVDF runqueues preserve their local invariants
- a thread exists in at most one CPU runqueue
- balancing migrates only runnable entities and preserves global ownership

Suggested layout:

- `include/`: public C headers for the verified boundary
- `src/`: canonical C implementation linked by kernel and userland
- `spec/`: Coq model and API-level specifications
- `proof/`: VST proofs for the C implementation
- `tests/`: executable model, differential tests, fuzzers, trace replay
- `traces/`: captured trace fixtures and format notes

Current EEVDF C core:

- `include/pacha_eevdf.h`
- `src/pacha_eevdf.c`
- `include/pacha_sched.h`
- `src/pacha_sched.c`
- `include/pacha_kernel_sched.h`
- `src/pacha_kernel_sched.c`
- `tests/test_pacha_eevdf.c`
- `tests/test_pacha_eevdf_property.c`
- `tests/test_pacha_sched.c`
- `tests/test_pacha_kernel_sched.c`
- `tests/test_pacha_kernel_sched_property.c`
- `spec/EevdfTestVectors.v`
- `spec/SchedRuntimeModel.v`
- `spec/SchedRuntimeSpec.v`
- `spec/SchedRuntimeTestVectors.v`
- `spec/KernelSchedModel.v`
- `spec/KernelSchedInvariants.v`
- `spec/KernelSchedSpec.v`
- `spec/KernelSchedPreservation.v`
- `spec/KernelSchedTestVectors.v`

Run the kernel-scheduler pre-integration gate:

```sh
./verified/scheduling/verify-kernel-sched.sh
```

This builds and runs the C smoke/property tests with `clang
-std=c11 -Wall -Wextra -Werror`, then builds the EEVDF and kernel scheduler Coq
targets. If `coq_makefile` is not already available, the script falls back to
`nix develop` for the Coq part.

Build the current C smoke test into `.artifacts/`:

```sh
mkdir -p .artifacts/verified-scheduling
clang -std=c11 -Wall -Wextra -Werror \
  -Iverified/scheduling/include \
  verified/scheduling/src/pacha_eevdf.c \
  verified/scheduling/tests/test_pacha_eevdf.c \
  -o .artifacts/verified-scheduling/test_pacha_eevdf
.artifacts/verified-scheduling/test_pacha_eevdf
```

Build the deterministic property test:

```sh
clang -std=c11 -Wall -Wextra -Werror \
  -Iverified/scheduling/include \
  verified/scheduling/src/pacha_eevdf.c \
  verified/scheduling/tests/test_pacha_eevdf_property.c \
  -o .artifacts/verified-scheduling/test_pacha_eevdf_property
.artifacts/verified-scheduling/test_pacha_eevdf_property
```

Build the scheduler runtime smoke test:

```sh
clang -std=c11 -Wall -Wextra -Werror \
  -Iverified/scheduling/include \
  verified/scheduling/src/pacha_eevdf.c \
  verified/scheduling/src/pacha_sched.c \
  verified/scheduling/tests/test_pacha_sched.c \
  -o .artifacts/verified-scheduling/test_pacha_sched
.artifacts/verified-scheduling/test_pacha_sched
```

Build the kernel scheduler smoke test:

```sh
clang -std=c11 -Wall -Wextra -Werror \
  -Iverified/scheduling/include \
  verified/scheduling/src/pacha_eevdf.c \
  verified/scheduling/src/pacha_kernel_sched.c \
  verified/scheduling/tests/test_pacha_kernel_sched.c \
  -o .artifacts/verified-scheduling/test_pacha_kernel_sched
.artifacts/verified-scheduling/test_pacha_kernel_sched
```

Build the deterministic kernel scheduler property smoke test:

```sh
clang -std=c11 -Wall -Wextra -Werror \
  -Iverified/scheduling/include \
  verified/scheduling/src/pacha_eevdf.c \
  verified/scheduling/src/pacha_kernel_sched.c \
  verified/scheduling/tests/test_pacha_kernel_sched_property.c \
  -o .artifacts/verified-scheduling/test_pacha_kernel_sched_property
.artifacts/verified-scheduling/test_pacha_kernel_sched_property
```

`pacha_sched` is the older platform-independent scheduler runtime layer above
`pacha_eevdf`. It remains useful as an executable C model while the kernel
scheduler model is redesigned. Long-term kernel scheduling should follow
`spec/KernelSchedModel.v`: per-CPU EEVDF runqueues, per-CPU current-thread
bookkeeping, scheduler activation, and explicit migration.

`pacha_kernel_sched` is the C implementation shaped after
`spec/KernelSchedModel.v`. It keeps one EEVDF runqueue per CPU, tracks per-CPU
current thread and activation-pending state, exposes explicit transition
functions, and keeps trap-frame/IPI/context-switch work outside the verified
core. This is the implementation candidate for kernel integration before VST.
`pacha_kernel_sched_validate` is a C-side invariant checker mirroring the Coq
kernel scheduler invariants; smoke tests call it after transitions and also
check that corrupt states are rejected.

`spec/SchedRuntimeModel.v` is the Coq model for the C scheduler runtime API.
`spec/SchedRuntimeSpec.v` gives one Coq-level spec theorem per C runtime
function. `spec/SchedRuntimeTestVectors.v` mirrors `tests/test_pacha_sched.c`
lifecycle cases so the runtime behavior is represented on both sides before VST
work.

`spec/KernelSchedModel.v` is the new long-term abstract model for the in-kernel
scheduler. It composes CPU-local EEVDF runqueues with kernel-level current CPU
ownership, activation, timer/block/wake transitions, and runnable migration.
`spec/KernelSchedInvariants.v` records the cross-CPU invariants that the kernel
implementation must preserve. `spec/KernelSchedSpec.v` gives Coq-level function
specifications for the model transitions. `spec/KernelSchedPreservation.v`
starts the preservation layer with the empty state and failure/no-op paths,
successful activation request, duplicate add rejection, busy pick rejection, and
idle pick/claim success paths.
`spec/KernelSchedTestVectors.v` mirrors the kernel scheduler C smoke test cases
for per-CPU pick, duplicate rejection, and activation state.

`proof/SchedRuntimeVstSpec.v` is the first VST bridge: it imports VST, records
the C scalar bounds and enum encodings, names the model-to-memory representation
predicates that will become `data_at` layouts, and gives one logical VST-facing
postcondition per scheduler runtime API function.
`proof/generated/` is produced by `proof/gen-clight.sh` with `clightgen`.
The verified C ABI uses explicit result codes plus out-parameters instead of
returning structs by value, so generated Clight functions expose `tint`/`tvoid`
returns. `proof/PachaSchedVst.v` imports that AST, builds `CompSpecs`, and
defines `data_at` representations for scheduler structs.

The current model-correspondence layer is `spec/EevdfTestVectors.v`: it mirrors
the C boundary cases as Coq examples. It is intentionally simple for now, so it
can later grow into extraction-based differential tests, QuickChick, or VST-side
trace replay without changing the C core API.

VST preparation constraints for the C core:

- no dynamic allocation
- fixed-size entity table
- explicit result codes for all public transitions
- VST-facing public functions should use out-parameters instead of returning
  structs by value
- no `__int128` or compiler-specific integer extension in the core
- signed arithmetic goes through checked helpers before committing a result
- failed EEVDF transitions write the original runqueue copy to the out-parameter
- platform work remains outside the verified core

Kernel platform responsibilities remain outside this directory:

- trap frame save/restore
- CR3/PKRU/FS/GS handling
- AP wake and interrupt plumbing
- fd/event queue exposure
- actual context switch execution

Userland scheduling daemon responsibilities are no longer part of the long-term
design. Userland code may still consume diagnostics or replay traces, but it is
not the owner of scheduling policy.
