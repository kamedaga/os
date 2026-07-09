# Userland Personality / filed / kobox / termd Large Refactor Plan

## Goal

This refactor simplifies the userland service structure by redesigning the
interfaces between LPR, filed, storage/kobox backend code, termd, and LPR
supervisor.

The goal is not to split binaries for its own sake. `filed` and storage runtime
may remain in the same binary when that is the right engineering boundary. The
problem to fix is mixed responsibility: old wire payload shapes, giant dispatch
files, service-private headers leaking into LPR, implicit state ownership, and
rebuild paths that recompile musl libc for ordinary server changes.

Kernel ABI is out of scope.

## Non-Negotiable Direction

- v2 names such as `ipc_protocol_v2.h` are acceptable and intentional.
- Previous userland wire headers are not compatibility contracts.
- Do not add operations, payloads, or compatibility shims to the previous
  op-number spaces.
- When a service/client boundary moves to v2, delete the corresponding old
  header and remove all build/package references in the same change.
- No compatibility layer should preserve old op numbers just to avoid updating
  callers.
- `zig build-obj` must not be run from repo root.
- Kernel changes require a separate justification and user approval.

## Current Foundation Already Added

- Common v2 service header:
  - `userland/libipc/include/pacha/service_abi.h`
- v2 boundary headers:
  - `userland/filed/include/filed/ipc_protocol_v2.h`
  - `userland/koboxd/include/koboxd/control_protocol_v2.h`
  - `userland/koboxd/include/koboxd/storage_protocol_v2.h`
  - `userland/netd/include/netd/ipc_protocol_v2.h`
  - `userland/termd/include/termd/ipc_protocol_v2.h`
  - `userland/lpr_supervisor/include/lpr_supervisor/ipc_protocol_v2.h`
  - `userland/personality/include/personality/coordinator_protocol_v2.h`
  - `userland/personality/include/personality/lpr_client_abi.h`
- ABI layout test:
  - `tests/run-userland-service-abi-layout.sh`
  - `tests/userland_service_abi_layout.c`
- Dynamic native app build driver:
  - `musl/pachaos/build/build-app-dynamic.sh`
- First dynamic-link migrations:
  - `userland/filed_smoke`
  - `userland/lpr_supervisor`

## Interface / ABI Redesign

All service requests and replies use a common v2 header:

- magic
- ABI version
- service id
- op
- flags
- request id
- trace id
- payload size
- fd count
- reply status
- reply error domain
- reply result

Service-specific payloads must be responsibility-specific:

- VFS payloads must not also carry exec fd patch semantics.
- Exec payloads must not reuse path-open structs as a dumping ground.
- TTY context must be explicit and shared by TTY ops.
- Storage object ids and filed handle ids must remain distinct types.
- Diagnostics are first-class v2 ops, not ad hoc debug additions.

## Error Domains

Use separate domains so failures are attributable without guessing:

- ABI mismatch
- LPR translation
- filed VFS
- filed exec
- storage backend
- termd TTY
- dynamic loader

The error domain is part of the reply header. Error conveyor chains can carry
deeper frames, but the top-level service/domain must be visible immediately.

## filed / Storage / kobox Direction

`filed` remains the owner of VFS and exec policy.

Storage/kobox code may remain in the same final binary where that is desirable.
That is not the debugging problem. The important boundary is internal:

- VFS core owns vnode/mount/file/handle/path-walk state.
- Backend adapter owns storage object calls and tmpfs routing.
- Exec service owns native/LPR exec plan, fd inheritance, and bootstrap fd
  patching.
- Wire server owns decode/encode/reply only.
- Metrics, cache state, and error conversion belong to the subsystem that owns
  the state being measured.

`filed` must treat storage as a backend object API even when direct-linked.
Storage object ids must not be confused with filed handles.

## LPR Runtime Direction

`lpr_filed.c` should be dismantled.

Target modules:

- `lpr_fd_table`
- `lpr_vfs_client`
- `lpr_tty_client`
- `lpr_process_client`
- `lpr_signal_client`
- `lpr_pipe_event`
- `lpr_error`

LPR must not include service-private headers. The only public ABI it should see
is `personality/lpr_client_abi.h` plus narrow client helper APIs.

The LPR runtime should translate Linux syscalls into client requests; it should
not know filed dispatch internals, termd wire details, storage object layout, or
kobox internals.

## termd Direction

`termd` should be split by responsibility:

- core TTY policy:
  - PTMX
  - PTS
  - CTTY
  - handle lifetime
  - session/pgrp/signal policy
- backend adapters:
  - Linux tty island
  - virtio-console
  - null backend
- wire server:
  - v2 decode
  - v2 reply
  - error domain mapping
  - diagnostics

`filed` sees `termd` as a TTY endpoint provider. Linux tty island details must
not leak through filed-facing or LPR-facing interfaces.

## Dynamic Linking Direction

Server builds should stop using `build-smokes.sh` as the default path because it
rebuilds libc objects for ordinary server changes.

Use:

- `musl/pachaos/build/build-runtime.sh` for libc runtime artifacts.
- `musl/pachaos/build/build-app-dynamic.sh` for server/app relinks.

Dynamic server ELF requirements:

- PT_INTERP must be `/lib/ld-musl-x86_64.so.1`.
- The rootfs must include `/lib/libc.so` and `/lib/ld-musl-x86_64.so.1`.
- `libc.so` must not contain unresolved compiler helper or setjmp symbols.
- Server source changes must not rebuild musl libc.

Static builds remain fallback during migration, but not the desired server build
path.

Bootstrap binaries loaded by hand-written early loaders stay static until those
loader paths explicitly support PT_INTERP and `AT_BASE`.

## Migration Phases

### Phase 0: Guardrails

- Keep ABI layout tests passing.
- Keep dynamic hello, filed_smoke, and lpr_supervisor dynamic builds passing.
- Keep old service headers deleted once the boundary has moved to v2.
- Do not add compatibility shims for the previous op-number spaces.

### Phase 1: Dynamic Build Expansion

Move small and low-risk binaries first:

1. dynamic hello
2. `filed_smoke`
3. `lpr_supervisor`
4. non-bootstrap service smokes

Then evaluate:

1. `termd`
2. `filed`
3. storage/kobox-heavy services

Do not dynamic-link `seed0root` or `storage_boot` until their hand-written loader
paths handle PT_INTERP and dynamic loader auxv correctly.

### Phase 2: lpr_supervisor v2 Boundary

- Convert `lpr_supervisor` dispatch to common v2 header.
- Convert all callers in `seed0root`, `termd`, and LPR runtime.
- Removed the old LPR supervisor service header.
- Keep only `lpr_supervisor/ipc_protocol_v2.h`.
- Add ABI mismatch rejection tests.

### Phase 3: termd v2 Boundary

- Convert `termd` request/reply handling to v2.
- Introduce explicit `termd_v2_tty_context_t` use in all relevant calls.
- Convert filed and LPR TTY callers.
- Removed the old termd service header.
- Keep Linux tty island behind backend adapter APIs.

### Phase 4: filed v2 Boundary

- Introduce v2 decode layer in filed wire server.
- Route v2 ops to VFS, exec, service endpoint, and diagnostics subsystems.
- Convert seed0boot, seed0root, netd, LPR runtime, filed_smoke, and internal
  clients.
- Removed the old filed service header; keep filed payload layouts in `filed/payload_v2.h`
  until they are folded into narrower v2 responsibility headers.
- Delete old op-number assumptions and previous fast-doorbell semantics unless they
  are deliberately redesigned as v2 fast IPC.

### Phase 5: Storage v2 Boundary

- Convert storage backend adapter to `storage_protocol_v2.h`.
- Keep direct-link storage possible, but call through typed backend object APIs.
- Convert any IPC storage endpoint users.
- Removed the old koboxd service header; keep kobox endpoint discovery in
  `koboxd/control_protocol_v2.h`.
- Ensure storage object id and filed handle id cannot be mixed by type/API.

### Phase 6: LPR Runtime Module Split

- Create the target LPR client modules.
- Move logic out of monolithic `lpr_filed.c`.
- Keep `netd/ipc_protocol_v2.h` as the network syscall translation boundary;
  LPR must not include the old netd service header.
- Ensure LPR runtime source includes only LPR client ABI and client helpers
  directly; service v2 headers are centralized behind `lpr_client_abi.h`.
- Add fd table dump and client-state diagnostics.

## Diagnostics Requirements

Every service boundary must support diagnostics through v2:

- LPR fd table snapshot
- filed VFS/handle/cache snapshot
- filed exec plan/fd inheritance snapshot
- storage backend object/cache snapshot
- termd TTY/session snapshot
- dynamic loader failure context

Every diagnostic response must include service id, op, trace id, status, and
error domain.

## Required Tests

ABI tests:

- common header size/alignment
- payload struct size
- op range sanity
- previous header rejection

Build tests:

- `musl/pachaos/build/build-runtime.sh`
- `musl/pachaos/build/build-app-dynamic.sh`
- dynamic `filed_smoke`
- dynamic `lpr_supervisor`
- later dynamic `termd` and `filed`

Runtime tests:

- filed VFS tests
- LPR syscall smoke
- filed exec smoke
- termd PTMX/PTS/ioctl/poll smoke
- boot profile smoke

Regression tests:

- server source change must not rebuild musl libc
- ABI mismatch must fail clearly
- fd inheritance and bootstrap fd table must not drift unintentionally
- storage errors and VFS policy errors must remain separate
- TTY errors and LPR signal errors must remain separate

## Definition of Done

- All service boundaries use v2 headers.
- old service headers are deleted.
- LPR no longer includes service-private wire headers.
- Server builds use dynamic app driver where their loader path supports it.
- Static fallback remains available but is not the normal server development
  path.
- Diagnostics identify service, op, trace, status, and error domain without
  requiring log archaeology.
