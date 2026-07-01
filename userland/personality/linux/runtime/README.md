# Linux Personality Runtime

This directory will contain the per-process Linux Personality Runtime.

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
