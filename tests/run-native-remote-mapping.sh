#!/usr/bin/env bash
# Boot-only image: no rootfs disk is copied or attached.
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/native-remote-mapping"
bash "$repo_root/tests/build-native-remote-mapping.sh"
(cd "$repo_root/kernel" && zig build limine)
(cd "$repo_root/pack" && go run ./cmd/bootfs-image \
  --manifest "$repo_root/tests/kobox2-foundation.bootfs" --output "$out/bootfs.img")
cp --reflink=auto "$repo_root/.artifacts/limine-boot.img" "$out/boot.img"
mcopy -o -i "$out/boot.img@@2097152" "$repo_root/kernel/zig-out/bin/limine/pacha-kernel.elf" ::/KERNEL.ELF
mcopy -o -i "$out/boot.img@@2097152" "$out/native_remote_mapping.elf" ::/INITAPP.ELF
mcopy -o -i "$out/boot.img@@2097152" "$out/bootfs.img" ::/BOOTFS.IMG
sha256sum "$repo_root/kernel/zig-out/bin/limine/pacha-kernel.elf" "$out/native_remote_mapping.elf" >"$out/inputs.sha256"
: >"$out/serial.log"
qemu-system-x86_64 -machine q35 -cpu host -enable-kvm -m 2G -smp 2 \
  -device intel-iommu,intremap=off,aw-bits=48 \
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
  if rg -q 'NATIVE_REMOTE_MAPPING=(PASS|FAIL)|ACTION=terminate|PANIC|init ELF load failed' "$out/serial.log"; then break; fi
  sleep 0.2
done
stop_qemu
trap - EXIT
if ! rg -q 'NATIVE_REMOTE_MAPPING=PASS' "$out/serial.log"; then tail -100 "$out/serial.log"; exit 1; fi
if ! rg -q 'NATIVE_THREAD_CONTEXT=PASS' "$out/serial.log"; then tail -100 "$out/serial.log"; exit 1; fi
if ! rg -q 'NATIVE_REMOTE_LOW_RANGE=PASS' "$out/serial.log"; then tail -100 "$out/serial.log"; exit 1; fi
if rg -n 'NATIVE_REMOTE_MAPPING=FAIL|PANIC|ACTION=terminate' "$out/serial.log"; then exit 1; fi
printf 'Native remote mapping passed: %s\n' "$out/serial.log"
