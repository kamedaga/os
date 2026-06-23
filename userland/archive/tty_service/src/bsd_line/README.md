# FreeBSD TTY Line Discipline Port

This directory contains BSD-derived TTY line discipline code used by
`tty_service`.

## Source Snapshot

- Project: FreeBSD source tree
- Repository: https://github.com/freebsd/freebsd-src
- Ref: `refs/heads/main`
- Revision: `1d24638d3e8875e4b99a4b5e39f4241e37221b3d`
- Retrieved: 2026-05-08
- License target: file-level BSD-2-Clause unless noted otherwise

Use revision-pinned URLs when importing files:

- `https://github.com/freebsd/freebsd-src/blob/1d24638d3e8875e4b99a4b5e39f4241e37221b3d/sys/sys/ttyqueue.h`
- `https://github.com/freebsd/freebsd-src/blob/1d24638d3e8875e4b99a4b5e39f4241e37221b3d/sys/kern/tty_inq.c`
- `https://github.com/freebsd/freebsd-src/blob/1d24638d3e8875e4b99a4b5e39f4241e37221b3d/sys/kern/tty_outq.c`
- `https://github.com/freebsd/freebsd-src/blob/1d24638d3e8875e4b99a4b5e39f4241e37221b3d/sys/kern/tty_ttydisc.c`

## License Notes

- Keep the original license header in every imported source file.
- Record any imported file with a non-BSD-2-Clause header before using it.
- Do not mix GPL code into this directory.

## Initial Files

- `tty_queue.h`
  - Derived from `sys/sys/ttyqueue.h`.
  - Queue structs and queue API.

- `tty_inq.c`
  - Derived from `sys/kern/tty_inq.c`.
  - Input queue, canonicalization, quote bits, erase/reprint iteration.

- `tty_outq.c`
  - Derived from `sys/kern/tty_outq.c`.
  - Output queue.

- `tty_ttydisc.c`
  - Derived from `sys/kern/tty_ttydisc.c`.
  - Termios line discipline.

## Local Interface

Keep FreeBSD-derived files isolated from CapabilityOS IPC and endpoint code.
CapabilityOS-specific behavior must live in adapter files outside
`bsd_line/`.

Expected local hooks:

- allocation/free
- assert
- byte-span read/write
- output drain
- pending read wakeup
- signal event enqueue

## Initial Compatibility Target

The first BSD-derived implementation is intentionally narrow:

- canonical read returns a line after newline
- `VERASE` backspace
- `VKILL`
- `VEOF`
- `ECHO`, `ECHOE`, `ECHOK`, `ECHONL`
- `ICRNL`, `INLCR`, `IGNCR`
- `OPOST`, `ONLCR`
- raw mode with `VMIN=1`, `VTIME=0`

## Porting Rules

- Preserve original license headers in imported source files.
- Do not paste CapabilityOS service IPC into imported files.
- Do not route Linux or OpenBSD `struct termios` into this layer.
- Do not reintroduce line discipline policy into `virtio_console`.
- Record every imported source file and any deliberate semantic omission here.
- Prefer small compatibility shims over rewriting large BSD functions.
