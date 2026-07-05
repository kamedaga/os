# Linux 6.8 TTY Source Island

This directory contains the Linux 6.8 TTY sources selected for the planned
`termd` kobox/Linux compatibility island:

- `drivers/tty/tty_io.c`
- `drivers/tty/tty.h`
- `drivers/tty/n_tty.c`
- `drivers/tty/tty_ioctl.c`
- `drivers/tty/tty_ldisc.c`
- `drivers/tty/tty_buffer.c`
- `drivers/tty/tty_port.c`
- `drivers/tty/tty_mutex.c`
- `drivers/tty/tty_ldsem.c`
- `drivers/tty/tty_baudrate.c`
- `drivers/tty/tty_jobctrl.c`
- `drivers/tty/n_null.c`
- `drivers/tty/pty.c`
- `drivers/tty/hvc/hvc_console.c`
- `drivers/tty/hvc/hvc_console.h`
- `drivers/virtio/virtio.c`
- `drivers/virtio/virtio_anchor.c`
- `drivers/virtio/virtio_ring.c`
- `drivers/virtio/virtio_pci_common.c`
- `drivers/virtio/virtio_pci_common.h`
- `drivers/virtio/virtio_pci_modern_dev.c`
- `drivers/virtio/virtio_pci_legacy_dev.c`
- `drivers/virtio/virtio_pci_modern.c`
- `drivers/virtio/virtio_pci_legacy.c`
- `drivers/char/virtio_console.c`
- `fs/devpts/inode.c`

These files are imported from the upstream Linux kernel v6.8 tag.

The first integration step is to build them as kobox-loaded Linux modules,
not to reimplement PTY or line discipline behavior inside `termd`.
Use:

```sh
bash pack/scripts/build_linux_tty_ko.sh
```

This produces:

- `linux_tty_core.ko`
- `linux_tty_n_null.ko`
- `linux_virtio.ko`
- `linux_virtio_ring.ko`
- `linux_virtio_pci_modern_dev.ko`
- `linux_virtio_pci_legacy_dev.ko`
- `linux_virtio_pci.ko`
- `linux_virtio_console.ko`

`devpts` and `hvc_console` are linked into `linux_tty_core.ko` because Linux
does not export the `devpts_*` helpers used by `pty.c`, and HVC is a helper
layer consumed by virtio-console. `virtio` and `virtio_ring` are source-built
from Linux 6.8 because the distro packages checked so far ship them built in.
`virtio_pci_modern_dev`, `virtio_pci_legacy_dev`, `virtio_pci`, and
`virtio_console` are fetched as existing Arch Linux 6.8.0 `.ko` modules by
`pack/scripts/fetch_arch_linux_6_8_virtio_ko.sh`.
