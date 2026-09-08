# netd packet lifetime crash (2026-09-07)

## Reproduction

Installing `emacs-gtk3` followed by `gtk4.0-demo` reproduced the user's Xfce
session loss with the exact same native fault: RIP `0x101ab2cf`, CR2 `0`,
instruction `mov %rsi,(%r14)`. The native image is netd, not filed; the generic
principal name `fd-process` does not identify the executable.

Subtracting the PIE load bias `0x10000000` resolves the instruction to
`zone_free_item`, `sys/vm/uma_core.c:2996`, in the pinned libuinet archive.
This is empty-bucket/slab cleanup after an allocation failure.
netd also owns AF_UNIX sockets, including X11 connections, so its termination
disconnects the desktop. The later device/DMA faults follow the service exit;
they are not evidence that IOMMU caused this first fault.

## Two defects and their fixes

1. The packet descriptor initializer stores its context in `ext_arg2`, but
   `uinet_pd_mbuf_ext_free` reads `arg1`. That argument is NULL, and the
   allocator intentionally ignores freeing NULL. Each received packet leaks
   its descriptor/mbuf/cluster. Read the matching second callback argument.
2. `uma_startup` receives ordinary malloc memory, although its embedded slab
   lookup rounds item addresses down to a page boundary. Allocate its arena
   with the existing page-aligned host `uhi_mmap` adapter and zero it. This
   repairs the failure-path slab lookup rather than suppressing the fault.

Both changes are maintained by `pack/scripts/apply_libuinet_pachaos_overlay.sh`
and checked for successful application. No kernel, public ABI, Xfce, apk, or
other Linux application changes are involved. The existing netd pool limits
are not increased. Splitting external networking from local IPC is a separate
architectural task and has not been implemented here.

## Host regression

`bash tests/run-netd-libuinet-lifetime.sh` links the real netd libuinet archive.
It exercises failed initialization, bounded-pool exhaustion/reuse, and 100,000
synchronously discarded Ethernet frames with the SMALL profile's 4,096-cluster
limit. The fixture deliberately does not exercise TCP timers.

- Original archive: SIGSEGV in `zone_free_item`, same line as the guest fault.
- Alignment fix alone (RX-only diagnostic): no crash, but ENOBUFS at frame 4,032.
- Both fixes: failure cleanup/reuse and all 100,000 frames pass.
- Existing FD-budget, page-attachment and UNIX-socket host tests also pass.

The runner optionally accepts an archive path for before/after comparison.
Evidence is in `.artifacts/gtk4-crash/lifetime-*.log` and
`.artifacts/emacs-debug/gtk4-before8/`. Large historical disk copies have been
deleted; keep logs rather than regenerating those copies.

## Guest verification and artifact

The fixed native service is `.artifacts/gtk4-crash/netd-build/netd.elf`, SHA256
`0402165d89edbda163b7b4d1b0be7e0ae4f99b0260411bc6b479dbe9a528596d`.
It was staged using the guest's normal file operations into a 16 MiB qcow2
overlay (`.artifacts/emacs-debug/netd-stage8/changes.qcow2`) backed by the normal
disk. No whole-rootfs copy was made. The normal disk must remain unchanged
while this overlay is used. Restarting that overlay loads the replacement.

The `netd-fixed8` and `netd-fixed4` runs (8 / 4 vCPUs, both 4 GiB RAM)
removed the two packages, installed `emacs-gtk3` followed by `gtk4.0-demo`
with `apk --no-cache`, and flushed writes. Both reported `EMACS_APK_DONE=0`.
A newly opened terminal accepted input and emitted `EMACS_TERMINAL_ALIVE`
in both runs. The 4-vCPU run also confirmed the guest binary SHA256 above.
Neither run recorded a page fault, general protection fault, filesystem
storage fault, or out-of-memory error.

The shared test overlay grew to roughly 223 MiB and was deleted after these
checks. It must not be retained across writes to the backing normal disk.
Only logs/screenshots are retained. The tested build artifact is also copied
to `.artifacts/cmake/netd/netd.elf` for subsequent packaging.

Deployment completed via a normal guest boot (`netd-deploy`): download to
`/srv/netd.new`, verify its SHA256, set mode 0755, rename over `/srv/netd.elf`,
then sync. No test package installation/removal was applied to the normal
disk. The previous service binary remains in
`.artifacts/gtk4-crash/before-netd.elf` (about 8 MiB), not a disk backup.

A new small overlay of the deployed disk (`netd-deployed-smoke`) booted to
the wallpaper, printed the expected `/srv/netd.elf` SHA256, and accepted input
in a new terminal. It was deleted after verification. The temporary download
server was stopped. Both test-image directories contain no retained rootfs
images or qcow2 overlays; repository disk use remains about 27 GiB.
