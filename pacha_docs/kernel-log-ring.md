# Debug kernel log ring

The normal kernel keeps its serial output and one-time boot-log handoff. For
runtime diagnostics, build the Limine kernel with
`cd kernel && zig build limine -Dkernel-log-ring=true`. The option is off by
default and is independent of `-Dkernel-debug` (symbol retention).

The debug kernel retains the most recent 64 KiB of **kernel-originated** text.
Native syscall `KERNEL_LOG_READ` (slot 2, request version 1) copies at most
1024 bytes per call and returns the next/oldest cursors, an overrun flag and a
count of fragments dropped under writer contention. The syscall slot exists in
all builds; a kernel without the ring returns `NOT_READY`. The input header
and size are checked before a bounded snapshot; output is copied to user
memory after the ring lock is released. The ring never shares the global
kernel-state lock.

`logd` reads the ring only when enabled and publishes a separate, recent
64 KiB `/run/pacha/kernel.log` on the RAM root. User-originated `syscall_log`
and serial-FD writes remain on serial and are not inserted into that file.
`/run/pacha/service.log` remains the userland service log. A file read during
publication may see a partial snapshot; the cursor and overrun indication in
the native syscall are authoritative for loss detection.

This is a debug-build interface, not a production audit log: all native
processes can read it, it is volatile, and a contended kernel writer may drop
a fragment instead of blocking an interrupt/fault path. The added ABI slot
renumbered subsequent native syscalls, so a debug kernel must be deployed with
the matching rebuilt bootfs and init application, not swapped alone.

The matching live image was booted under QEMU with AMD IOMMU and reached SSH.
`/run/pacha/kernel.log` contained kernel boot and IOMMU lines (918 bytes),
while user-originated `seed0boot` messages stayed out of that file. The
physical-board cache-transition reason still needs a fresh boot and probe;
QEMU does not reproduce that failure.
