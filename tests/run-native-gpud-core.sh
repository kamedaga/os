#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Boot-only fixture: no rootfs copy, synchronization or attachment.
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/native-gpud-core"
manifest="$repo_root/tests/kobox2-gpud-core.bootfs"
qemu_devices=()
case "${1:-}" in
  "") [[ $# == 0 ]] || exit 2 ;;
  --device)
    [[ $# == 1 ]] || exit 2
    out="$repo_root/.artifacts/tests/native-gpud-device"
    manifest="$repo_root/tests/kobox2-gpud-device.bootfs"
    qemu_devices=(-device virtio-gpu-pci,disable-legacy=on,iommu_platform=on)
    ;;
  *) exit 2 ;;
esac
bash "$repo_root/tests/build-native-gpud-core.sh" "$@"
(cd "$repo_root/kernel" && zig build limine)
(cd "$repo_root/pack" && go run ./cmd/bootfs-image \
  --manifest "$manifest" --output "$out/bootfs.img")
cp --reflink=auto "$repo_root/.artifacts/limine-boot.img" "$out/boot.img"
mcopy -o -i "$out/boot.img@@2097152" "$repo_root/kernel/zig-out/bin/limine/pacha-kernel.elf" ::/KERNEL.ELF
mcopy -o -i "$out/boot.img@@2097152" "$out/parent.elf" ::/INITAPP.ELF
mcopy -o -i "$out/boot.img@@2097152" "$out/bootfs.img" ::/BOOTFS.IMG
sha256sum "$repo_root/kernel/zig-out/bin/limine/pacha-kernel.elf" \
  "$out/parent.elf" "$out/child.elf" "$out/bootfs.img" >"$out/inputs.sha256"
: >"$out/serial.log"
qemu-system-x86_64 -machine q35 -cpu host -enable-kvm -m 2G -smp 2 \
  -device intel-iommu,intremap=off,aw-bits=48 \
  "${qemu_devices[@]}" \
  -drive "file=$out/boot.img,format=raw,if=ide" -display none \
  -serial "file:$out/serial.log" -monitor none -no-reboot >"$out/qemu.log" 2>&1 &
qemu_pid=$!
stop_qemu() {
  if kill -0 "$qemu_pid" 2>/dev/null; then kill "$qemu_pid"; fi
  wait "$qemu_pid" || true
}
trap stop_qemu EXIT
deadline=$((SECONDS + 420))
while kill -0 "$qemu_pid" 2>/dev/null && [[ "$SECONDS" -lt "$deadline" ]]; do
  if rg -q 'NATIVE_GPUD_CORE=PASS|=FAIL|ACTION=terminate|PANIC|init ELF load failed' "$out/serial.log"; then break; fi
  sleep 0.2
done
stop_qemu
trap - EXIT
if rg -n '=FAIL|ACTION=terminate|PANIC|init ELF load failed' "$out/serial.log" || \
   ! rg -q 'NATIVE_GPUD_CORE=PASS' "$out/serial.log"; then
  tail -100 "$out/serial.log"
  exit 1
fi
gates=(NATIVE_GPUD_CORE_ROUND NATIVE_GPUD_CORE_PACKAGE NATIVE_GPUD_MODULE_LIFECYCLE PACHA_KOBOX_NATIVE_MAPPING
  PACHA_KOBOX_BOOT_SMP PACHA_KOBOX_MEMORY PACHA_KOBOX_WAIT PACHA_KOBOX_RCU
  PACHA_KOBOX_WORKQUEUE PACHA_KOBOX_CLEANUP PACHA_KOBOX_FOUNDATION
  PACHA_KOBOX_VFS PACHA_KOBOX_SHMEM PACHA_KOBOX_PRESSURE PACHA_KOBOX_ALLOCATION
  PACHA_KOBOX_CLIENT_TASK PACHA_KOBOX_CHAPTER2_CORE)
if [[ "${1:-}" == --device ]]; then
  gates+=(NATIVE_GPUD_DEVICE_GRANT NATIVE_GPUD_DEVICE_LIFECYCLE NATIVE_GPUD_DRM_FILE
    NATIVE_GPUD_GPU_DISPATCH NATIVE_GPUD_GPU_QUERY NATIVE_GPUD_GPU_SESSION)
fi
for gate in "${gates[@]}"; do
  if [[ "$(rg -c "${gate}=PASS" "$out/serial.log")" != 2 ]]; then
    printf 'Expected two completed generations for %s\n' "$gate" >&2
    exit 1
  fi
done
if nm "$out/child.elf" | rg ' [Tt] (kb2_controller_|kb2_closure_builder_|gpud_)'; then
  echo 'Controller code leaked into the GPL child' >&2
  exit 1
fi
printf 'Native gpud core boot passed: %s\n' "$out/serial.log"
