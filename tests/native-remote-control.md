# Native remote memory and execution contracts

The kobox2 adapter owns Linux VMA/PTE selection and page lifetime. A native
process capability only authorizes mapping/invalidating a selected range; the
kernel does not interpret Linux VM or syscall semantics. The RAM VMO capability
does not need to be granted to the target process.

`PROCESS_MAP_REPLACE` is a flag of `PROCESS_MAP`, requires an explicit address,
and replaces that range synchronously. It is rejected by `PROCESS_MAP_BATCH`.
`PROCESS_UNMAP` takes `(process_fd, va, size, flags=0)` and requires `MAP_INTO`.
Both work on stopped processes and invalidate old translations before releasing
the old mapping. Source capability rights remain the VMA protection ceiling:
the caller must attenuate the source FD to the precise allowed protections if
the target must not elevate them with its own native `MPROTECT`.

## Native thread context

`THREAD_CONTEXT` is placed immediately after `THREAD_START`, at syscall 8.
All subsequent native syscall numbers were shifted; old binaries must be rebuilt.
Arguments are `(thread_fd, op, context_pointer, context_size)` in the usual
native syscall registers. GET is 0 and requires `INSPECT`; SET is 1 and requires
`SET_CONTEXT`. The common C definition and wrappers are in `pacha/ipc.h`.

The 1056-byte architectural image contains 64 bytes of metadata (size, feature
mask, FS base, GS base, PKRU, three reserved words), 160 bytes of GPR/IRET frame,
and an 832-byte standard x87/SSE/AVX XSAVE image. Feature mask is 7. SET validates
the complete image before committing; user selectors, canonical addresses,
reserved metadata, XSAVE header and MXCSR must be valid. Privileged RFLAGS are
cleared and IF is forced on. Neither operation starts or continues the target.

The target must be CPU-quiescent and suspended (freshly created or stopped).
Runnable/CPU-owned threads and active kernel wait/fault lifecycles return
`NOT_READY`. Generation and owner are checked again under the scheduler locks.
The API does not cancel a wait, consume a fault, or change notification state.
The caller must serialize its own STOP/context/CONTINUE sequence against other
holders of start/control authority. STOP followed by GET is not one transaction
against another authorized controller. Native fault snapshots remain the fault
upcall's saved frame rather than an externally rewritten in-flight handler.

Kernel involvement is required because a CPU-bound target need not execute a
cooperative worker or syscall, and userland cannot atomically inspect or replace
its saved privileged scheduler frame/XSAVE/TLS state. Linux register translation,
clone policy, signal interpretation, and LPR syscall interception remain in
userland. A new process with selected mappings and a suspended thread can be
initialized from a context using SET then START; no remote Linux fork operation
was added.

## Verification

Run `bash tests/run-native-remote-mapping.sh` from the repository root. It builds
the kernel and a freestanding native test, copies only the Limine boot image
into `.artifacts/tests/native-remote-mapping`, and attaches no rootfs disk.

The two target processes have empty capability tables. Tests cover repeated
shared/default/offset replacement, neighboring and peer mappings, source FD
close lifetime, access faults after unmap, rights/offset/flag failures preserving
old mappings, and STOP/unmap/replace/CONTINUE. Context tests include initial
suspended GET/SET/START, 32 CPU-bound STOP/SET/CONTINUE cycles with GPR, XMM15,
FS and GS verification, separate INSPECT/SET_CONTEXT rights, malformed image
failure atomicity, RFLAGS sanitization, and terminal-thread rejection. Scheduler
unit tests cover generation/owner mismatch, CPU ownership, ready state and
active wait/fault rejection while preserving wait/stop lifecycle fields.

Successful serial markers are `NATIVE_THREAD_CONTEXT=PASS` and
`NATIVE_REMOTE_MAPPING=PASS`. `inputs.sha256` records the actual kernel and test
ELFs. These native contracts do not by themselves establish all kobox2 chapter
2, LPR syscall, FD, or Linux clone Gates.
