# Linux Personality Loader

The loader prepares a Linux process before the Linux interpreter runs.

Responsibilities:

- map the LPR ET_DYN object
- apply minimal LPR relocations
- create the runtime page
- create the zpoline low page
- reserve the low-address guard hole after the zpoline page
- map PT_INTERP
- patch executable mappings
- build Linux stack and auxv

Low address layout:

- `0x00000000..0x00000fff`: executable zpoline page
- `0x00001000..0x03ffffff`: no-access guard hole
- `0x04000000..`: normal Linux mappings

The loader should apply this layout before mapping the Linux main executable or
interpreter. Linux mappings that overlap the reserved low range must be rejected
or moved by a later text-clone/rebase path.

The zpoline page contains process-specific bytes because the final jump target is
the child-process VA of `lpr_syscall_entry`. Callers should use
`lpr_loader_install_low_layout()` after the LPR image is mapped and symbol
addresses are known.
