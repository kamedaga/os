# Linux Personality Runtime

This directory will contain the per-process Linux Personality Runtime.

The runtime is loaded into each Linux process address space and owns:

- zpoline syscall entry
- process-local runtime state
- fd cache
- brk/mmap bookkeeping
- coordinator client

The runtime must be freestanding and must not depend on libc or the Linux
dynamic linker.
