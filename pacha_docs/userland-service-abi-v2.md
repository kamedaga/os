# Userland Service ABI v2

## Policy

The v2 suffix is intentional. It marks the current userland service ABI
generation.

The previous wire headers are not compatibility contracts. They have been
removed from the active service/client boundaries instead of being kept as
compatibility shims.

## Migration Rule

- Add or change service behavior only in the v2 headers.
- Convert all callers and servers for one service boundary together.
- Do not reintroduce compatibility shims for the previous op-number spaces.
- Keep `*_v2.h` names until another ABI generation exists.

## Current Boundaries

- `filed/ipc_protocol_v2.h`: filed namespace, handles, file I/O, exec, service
  endpoint registration, diagnostics.
- `koboxd/control_protocol_v2.h`: kobox endpoint discovery and endpoint reply
  envelope.
- `koboxd/storage_protocol_v2.h`: storage backend object operations.
- `netd/ipc_protocol_v2.h`: socket endpoint operations used by LPR network
  syscall translation.
- `termd/ipc_protocol_v2.h`: TTY objects, PTMX/PTS/CTTY, I/O, ioctl, poll,
  signal, diagnostics.
- `lpr_supervisor/ipc_protocol_v2.h`: Linux process tree, wait, signal, fd
  metadata.
- `personality/lpr_client_abi.h`: the ABI LPR runtime code is allowed to see.
