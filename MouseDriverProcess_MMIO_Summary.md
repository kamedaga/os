# MouseDriverProcess + MMIO Capability Migration Summary

## Goal
- Move virtio-input handling out of kernel into a user process (`MouseDriverProcess`).
- Add minimal syscall support for user MMIO mapping.
- Remove kernel-resident `virtio_input` driver implementation.
- Add a separate mouse drawing process that renders cursor movement to framebuffer.

## Implemented Changes

### 1) New syscall: MMIO page map (`int 0x80`, `rax=0xB`)
- File: `kernel/src/main.zig`
- Added `syscall_map_mmio = 0xB`.
- Dispatch path maps `(va=rdi) -> (paddr=rsi)` with writable flag from `rdx & 1`.
- Mapping still goes through capability checks (`capability.mapUserPageFromCapability`), so only pages with proper caps/rights can be mapped.

### 2) Capability runtime physical mapping limit update
- File: `kernel/src/capability.zig`
- `RuntimeConfig` field renamed:
  - from `four_gib`
  - to `physical_map_limit`
- `mapUserPageFromCapability` now validates `paddr < physical_map_limit`.
- File: `kernel/src/main.zig`
  - Capability runtime init now sets `.physical_map_limit = one_tib`.
  - This enables mapping high MMIO pages (for modern virtio BAR addresses like `0xC000000000`).

### 3) MouseDriverProcess boot integration
- File: `kernel/src/main.zig`
- Added disk path/load for `\\EFI\\BOOT\\MOUSEDRV.ELF`.
- Added probe-only flow using `virtio_probe.zig`:
  - Probe modern virtio-input PCI caps in kernel.
  - Build page+offset config for common/notify/isr/device areas.
- Added Process0 setup (when boot-log console mode is enabled):
  - Allocate user code page + ELF tail page + runtime stack page + config page.
  - Build Process0 user page table.
  - Map config page at user VA `0x20003000`.
  - Map runtime stack at `0x20002000`.
  - Install MMIO capabilities into Process0 (`KernelState.installCap`).
  - Publish config data into config page (magic + MMIO page addresses + offsets + notify multiplier).
  - Load `MOUSEDRV.ELF` with two-page loader and set Thread0 RIP/RSP.

### 4) Separate mouse draw process (Process1)
- File: `kernel/src/main.zig`
- Added disk path/load for `\\EFI\\BOOT\\MDRAW.ELF`.
- Added shared page at user VA `0x2000A000` (mapped into Process0 and Process1).
- Kernel publishes shared state:
  - magic, framebuffer width/height/pitch
  - cursor x/y, buttons, sequence
- Process1 now loads `MDRAW.ELF` (instead of BootLogConsole when mouse mode is enabled).
- Framebuffer remains mapped to Process1 at `0x20004000`.

### 5) Mouse userspace programs
- File: `kernel/user_programs/mouse_driver.zig`
  - In addition to virtio polling, now updates shared cursor state page for renderer process.
  - Logs include absolute cursor coordinates.
- File: `kernel/user_programs/mouse_draw.zig` (new)
  - Polls shared page sequence.
  - Draws cursor rectangle onto framebuffer.
  - Erases previous cursor position and redraws at current position.

### 6) Build + disk image pipeline update
- File: `kernel/build.zig`
  - Added PIE freestanding executable `MOUSEDRV`.
  - Installs to `EFI/BOOT/MOUSEDRV.ELF`.
  - Added PIE freestanding executable `MDRAW`.
  - Installs to `EFI/BOOT/MDRAW.ELF`.
  - Hooked into `zig build efi` install dependency chain.
- File: `setupDisk.sh`
  - Added copy step for `kernel/zig-out/bin/EFI/BOOT/MOUSEDRV.ELF`.
  - Added copy step for `kernel/zig-out/bin/EFI/BOOT/MDRAW.ELF`.

### 7) Kernel virtio-input removal
- File removed: `kernel/src/virtio_input.zig`
- File: `kernel/src/main.zig`
  - Removed `@import("virtio_input.zig")`.
  - Removed kernel driver instance and timer-time `poll()`.
  - Removed kernel-side queue page alloc/init path for virtio-input driver.
- Kernel now does probe-only for handoff; runtime event handling is in user process.

## Validation
- `zig build efi` passed.
- `zig build test` passed.

## Notes
- Current integration runs:
  - Process0: MouseDriverProcess
  - Process1: MouseDrawProcess (framebuffer renderer)
- If probe fails, mouse process is disabled and kernel logs that condition.
- Recommended QEMU input device for host/guest cursor sync without manual grab:
  - `-device virtio-tablet-pci`
