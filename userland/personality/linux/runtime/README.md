# Linux Personality Runtime

This directory contains the per-process Linux Personality Runtime.

An application using LPR is still an ordinary PachaOS process. PachaOS does not
give it a Linux-specific process type or place LPR in a privileged relationship
with the kernel. LPR is a dynamically linked user-space syscall abstraction: it
translates Linux ABI operations into native syscalls and userland service calls.

The application's authority is exactly the set of native FD capabilities
installed in that process. Bypassing or modifying LPR cannot create authority;
the kernel enforces the rights on every native FD operation. LPR provides Linux
semantics, not the OS security boundary.

The runtime is loaded into each Linux process address space and owns:

- zpoline syscall entry
- process-local runtime state
- fd cache
- brk/mmap bookkeeping
- coordinator client

The runtime is private to LPR. Its dynamic symbol table must not export runtime
internals into the guest Linux namespace; `build-lpr.sh` enforces this with
`check-lpr-namespace.sh`.

Small libc primitives used by LPR should be imported from musl and renamed into
private `lpr_*` symbols. Guest-visible libc state such as Linux `errno`, TLS,
stdio, and malloc must not be shared with LPR internals.

`support/` contains the private libc-like substrate used by runtime code:

- `arena.*`: LPR internal scratch arena helpers; not a guest-visible malloc
- `elf.*`: allocation-free ELF64 dynamic metadata parser
- `string.*`: musl-derived `lpr_mem*` and `lpr_str*`
- `syscall.*`: raw PachaOS syscall wrappers
