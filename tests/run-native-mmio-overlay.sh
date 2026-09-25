#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Boot-only device/adapter test; no rootfs disk is copied or attached.
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/native-mmio-overlay"
mkdir -p "$out"
"${CAPOS_FREESTANDING_CC:-clang}" -target x86_64-linux-gnu -fuse-ld=lld \
  -nostdlib -static -ffreestanding -fno-builtin -fno-stack-protector -mno-red-zone \
  -O2 -g -Wall -Wextra -Werror \
  -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/libipc/include" \
  -I "$repo_root/userland/libcapsule/include" -I "$repo_root/kobox2/linux-sandbox/kobox" \
  "$repo_root/tests/native_mmio_overlay.c" \
  "$repo_root/userland/kobox2_adapter/device_pci.c" \
  "$repo_root/userland/libpacha/src/syscall.c" \
  -Wl,-e,_start,--image-base=0x400000,-z,noexecstack -o "$out/native_mmio_overlay.elf"
(cd "$repo_root/kernel" && zig build limine)
(cd "$repo_root/pack" && go run ./cmd/bootfs-image \
  --manifest "$repo_root/tests/kobox2-foundation.bootfs" --output "$out/bootfs.img")
cp --reflink=auto "$repo_root/.artifacts/limine-boot.img" "$out/boot.img"
mcopy -o -i "$out/boot.img@@2097152" "$repo_root/kernel/zig-out/bin/limine/pacha-kernel.elf" ::/KERNEL.ELF
mcopy -o -i "$out/boot.img@@2097152" "$out/native_mmio_overlay.elf" ::/INITAPP.ELF
mcopy -o -i "$out/boot.img@@2097152" "$out/bootfs.img" ::/BOOTFS.IMG
sha256sum "$repo_root/kernel/zig-out/bin/limine/pacha-kernel.elf" \
  "$out/native_mmio_overlay.elf" >"$out/inputs.sha256"
: >"$out/serial.log"
qemu-system-x86_64 -machine q35 -cpu host -enable-kvm -m 2G -smp 2 \
  -device intel-iommu,intremap=off,aw-bits=48 \
  -device virtio-rng-pci,disable-legacy=on,iommu_platform=on \
  -drive "file=$out/boot.img,format=raw,if=ide" -display none \
  -serial "file:$out/serial.log" -monitor none -no-reboot -net none >"$out/qemu.log" 2>&1 &
qemu_pid=$!
stop_qemu() {
  if kill -0 "$qemu_pid" 2>/dev/null; then kill "$qemu_pid"; fi
  wait "$qemu_pid" || true
}
trap stop_qemu EXIT
deadline=$((SECONDS + 90))
while kill -0 "$qemu_pid" 2>/dev/null && [[ "$SECONDS" -lt "$deadline" ]]; do
  if rg -q 'NATIVE_MMIO_OVERLAY=PASS|NATIVE_DEVICE_CONTRACT=FAIL|ACTION=terminate|PANIC|panic|init ELF load failed' "$out/serial.log"; then break; fi
  sleep 0.2
done
stop_qemu
trap - EXIT
if ! rg -q 'NATIVE_MMIO_OVERLAY=PASS' "$out/serial.log"; then tail -100 "$out/serial.log"; exit 1; fi
if rg -n 'NATIVE_DEVICE_CONTRACT=FAIL|PANIC|panic|ACTION=terminate' "$out/serial.log"; then exit 1; fi
printf 'Native MMIO overlay passed: %s\n' "$out/serial.log"
