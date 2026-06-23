# Verified Scheduling

This directory is the canonical implementation home for scheduling logic that
is shared by the kernel and `userland/schedulerd`.

The code here is intended to be linked directly by both sides after it has a
matching formal model and proof story. Kernel and userland code should not fork
or reimplement this logic; they should adapt their platform state to this API
and execute the decisions returned by it.

The scheduling implementation is split into two layers:

- EEVDF core: runqueue ordering, charging, eligibility, and pick rules
- Protocol core: thread ownership, CPU ownership, commit/claim/preempt/block/wake
  state transitions

The verified implementation must not know about:

- kernel trap frames
- CR3, PKRU, FS/GS MSRs
- AP startup or IPIs
- file descriptors or syscalls
- serial output or logging
- kernel/userland allocation strategies

Those belong in adapters:

- kernel adapter: `kernel/src/...`
- userland adapter: `userland/schedulerd/src/...`

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
- `tests/test_pacha_eevdf.c`
- `tests/test_pacha_eevdf_property.c`
- `tests/test_pacha_sched.c`
- `spec/EevdfTestVectors.v`
- `spec/SchedRuntimeModel.v`
- `spec/SchedRuntimeSpec.v`
- `spec/SchedRuntimeTestVectors.v`

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

`pacha_sched` is the platform-independent scheduler runtime layer above
`pacha_eevdf`. It owns CPU current-thread bookkeeping, accepts thread/timer
events, and returns decisions for the kernel or userland adapter to execute. It
does not own trap frames, address spaces, fd state, logging, IPIs, or context
switch execution.

`spec/SchedRuntimeModel.v` is the Coq model for the C scheduler runtime API.
`spec/SchedRuntimeSpec.v` gives one Coq-level spec theorem per C runtime
function. `spec/SchedRuntimeTestVectors.v` mirrors `tests/test_pacha_sched.c`
lifecycle cases so the runtime behavior is represented on both sides before VST
work.

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

Kernel-owned responsibilities remain outside this directory:

- trap frame save/restore
- CR3/PKRU/FS/GS handling
- AP wake and interrupt plumbing
- fd/event queue exposure
- actual context switch execution

Userland-owned responsibilities remain outside this directory:

- daemon lifecycle
- event fd reads
- commit ioctl writes
- configuration and diagnostics
