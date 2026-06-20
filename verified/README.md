# Verified Code

This tree contains implementation code that is intended to be the canonical
source for verified logic shared by the kernel and userland.

Code in this tree is not kernel-owned and not userland-owned. Platform-specific
work such as syscalls, trap frames, CR3 switching, AP wakeups, fd I/O, logging,
and allocation stays in adapters outside this tree.

The rule for verified shared code is:

- one C implementation is linked by both kernel and userland
- one public C API describes the verified boundary
- Coq/VST specifications describe the same API
- adapters translate platform state into verified inputs
- verified code owns the scheduling logic
- adapters perform side effects requested by verified decisions

Current areas:

- `scheduling/`: EEVDF and scheduler protocol implementation
