#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Bootloader-only image; no rootfs copy or Linux runtime in this native test.
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/native-kobox2-irq"
adapter="$repo_root/userland/kobox2_adapter"
mkdir -p "$out"
"${CAPOS_FREESTANDING_CC:-clang}" -target x86_64-linux-gnu -fuse-ld=lld \
  -nostdlib -static-pie -ffreestanding -fno-builtin -fno-stack-protector -fPIE -mno-red-zone \
  -std=c11 -O2 -g -Wall -Wextra -Werror \
  -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/libipc/include" \
  -I "$repo_root/userland/libcapsule/include" -I "$repo_root/kobox2/linux-sandbox/kobox" \
  "$repo_root/tests/native_kobox2_irq.c" "$repo_root/userland/libpacha/src/syscall.c" \
  "$adapter/device_irq.c" "$adapter/device_irq_queue.c" \
  "$adapter/runtime.c" "$adapter/native.c" "$adapter/entry.S" \
  -Wl,-e,_start,-z,noexecstack -o "$out/native_kobox2_irq.elf"
(cd "$repo_root/kernel" && zig build limine)
(cd "$repo_root/pack" && go run ./cmd/bootfs-image \
  --manifest "$repo_root/tests/kobox2-foundation.bootfs" --output "$out/bootfs.img")
cp --reflink=auto "$repo_root/.artifacts/limine-boot.img" "$out/boot.img"
mcopy -o -i "$out/boot.img@@2097152" "$repo_root/kernel/zig-out/bin/limine/pacha-kernel.elf" ::/KERNEL.ELF
mcopy -o -i "$out/boot.img@@2097152" "$out/native_kobox2_irq.elf" ::/INITAPP.ELF
mcopy -o -i "$out/boot.img@@2097152" "$out/bootfs.img" ::/BOOTFS.IMG
sha256sum "$repo_root/kernel/zig-out/bin/limine/pacha-kernel.elf" \
  "$out/native_kobox2_irq.elf" >"$out/inputs.sha256"
sha256sum "$repo_root/tests/native_kobox2_irq.c" "$repo_root/tests/native_device_contract.c" \
  "$repo_root/tests/run-native-kobox2-irq.sh" "$adapter/device_irq.c" "$adapter/device_irq.h" \
  "$adapter/device_irq_queue.c" "$adapter/device_irq_queue.h" \
  "$adapter/runtime.c" "$adapter/native.c" "$adapter/entry.S" >"$out/sources.sha256"
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
deadline=$((SECONDS + 120))
while kill -0 "$qemu_pid" 2>/dev/null && [[ "$SECONDS" -lt "$deadline" ]]; do
  if rg -q 'NATIVE_KOBOX2_IRQ=PASS|NATIVE_DEVICE_CONTRACT=FAIL|PACHA_KOBOX_FOUNDATION=FAIL|ACTION=terminate|PANIC|panic|init ELF load failed' "$out/serial.log"; then break; fi
  sleep 0.2
done
stop_qemu
trap - EXIT
if ! rg -q 'NATIVE_KOBOX2_IRQ=PASS' "$out/serial.log"; then tail -100 "$out/serial.log"; exit 1; fi
if rg -n 'NATIVE_DEVICE_CONTRACT=FAIL|PACHA_KOBOX_FOUNDATION=FAIL|PANIC|panic|ACTION=terminate' "$out/serial.log"; then exit 1; fi
printf 'Native kobox2 IRQ adapter passed: %s\n' "$out/serial.log"
