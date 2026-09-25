#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Boot-only machine fixture; no rootfs copy, synchronization or attachment.
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/native-kobox2-ipc"
bash "$repo_root/tests/build-native-kobox2-ipc.sh"
(cd "$repo_root/kernel" && zig build limine)
(cd "$repo_root/pack" && go run ./cmd/bootfs-image \
  --manifest "$repo_root/tests/kobox2-ipc.bootfs" --output "$out/bootfs.img")
cp --reflink=auto "$repo_root/.artifacts/limine-boot.img" "$out/boot.img"
mcopy -o -i "$out/boot.img@@2097152" "$repo_root/kernel/zig-out/bin/limine/pacha-kernel.elf" ::/KERNEL.ELF
mcopy -o -i "$out/boot.img@@2097152" "$out/native_kobox2_ipc.elf" ::/INITAPP.ELF
mcopy -o -i "$out/boot.img@@2097152" "$out/bootfs.img" ::/BOOTFS.IMG
sha256sum "$repo_root/kernel/zig-out/bin/limine/pacha-kernel.elf" \
  "$out/native_kobox2_ipc.elf" "$out/child.elf" >"$out/inputs.sha256"
sha256sum "$repo_root/userland/kobox2_adapter/ipc.c" \
  "$repo_root/userland/gpud/process_native.c" "$repo_root/userland/gpud/process_native.h" \
  "$repo_root/userland/gpud/launch_native.c" "$repo_root/userland/gpud/launch_native.h" \
  "$repo_root/userland/gpud/launch.c" "$repo_root/userland/gpud/launch.h" \
  "$repo_root/userland/gpud/process.c" "$repo_root/userland/gpud/process.h" \
  "$repo_root/kobox2/src/controller/controller.c" "$repo_root/kobox2/src/closure.c" \
  "$repo_root/kobox2/src/closure_internal.h" "$repo_root/kobox2/include/kobox2/controller.h" \
  "$repo_root/kobox2/include/kobox2/closure.h" "$repo_root/tests/gpud_controller_fixture.h" \
  "$repo_root/musl/upstream/src/stdlib/qsort.c" "$repo_root/musl/upstream/src/stdlib/qsort_nr.c" \
  "$repo_root/musl/upstream/src/internal/atomic.h" "$repo_root/musl/upstream/arch/pachaos/atomic_arch.h" \
  "$out/musl/obj/src/stdlib/qsort.o" "$out/musl/obj/src/stdlib/qsort_nr.o" \
  "$repo_root/userland/gpud/launch_image.h" "$repo_root/userland/gpud/arch/x86_64/launch_image.c" \
  "$repo_root/userland/kobox2_adapter/ipc.h" \
  "$repo_root/userland/kobox2_adapter/bootstrap.c" "$repo_root/userland/kobox2_adapter/bootstrap.h" \
  "$repo_root/userland/kobox2_adapter/package.c" "$repo_root/userland/kobox2_adapter/package.h" \
  "$repo_root/kobox2/linux-sandbox/kobox/boot/package.c" \
  "$repo_root/kobox2/linux-sandbox/kobox/arch/x86_64/elf.c" \
  "$repo_root/kobox2/protocol/src/closure_manifest.c" \
  "$repo_root/kobox2/protocol/src/resource_grant.c" "$repo_root/kobox2/protocol/src/sha256.c" \
  "$repo_root/userland/seed0boot/src/bootfs_reader.c" \
  "$repo_root/userland/seed0boot/src/bootfs_reader.h" \
  "$repo_root/userland/seed0boot/src/bootstrap_abi.h" \
  "$repo_root/tests/native_kobox2_ipc.c" "$repo_root/tests/native_kobox2_ipc_child.c" \
  "$repo_root/tests/native_kobox2_ipc_common.h" "$repo_root/tests/native_kobox2_ipc_child.ld" \
  "$repo_root/tests/native_test_init.c" \
  "$repo_root/userland/libpacha/src/syscall.c" "$repo_root/userland/libpacha/include/pacha/abi.h" \
  "$repo_root/userland/libipc/include/pacha/ipc.h" \
  "$repo_root/tests/build-native-kobox2-ipc.sh" "$repo_root/tests/run-native-kobox2-ipc.sh" \
  "$repo_root/tests/kobox2-ipc.bootfs" >"$out/source-inputs.sha256"
: >"$out/serial.log"
qemu-system-x86_64 -machine q35 -cpu host -enable-kvm -m 2G -smp 2 \
  -device intel-iommu,intremap=off,aw-bits=48 \
  -drive "file=$out/boot.img,format=raw,if=ide" -display none \
  -serial "file:$out/serial.log" -monitor none -no-reboot \
  -netdev user,id=net0 -device virtio-net-pci,netdev=net0 >"$out/qemu.log" 2>&1 &
qemu_pid=$!
stop_qemu() {
  if kill -0 "$qemu_pid" 2>/dev/null; then kill "$qemu_pid"; fi
  wait "$qemu_pid" || true
}
trap stop_qemu EXIT
deadline=$((SECONDS + 90))
while kill -0 "$qemu_pid" 2>/dev/null && [[ "$SECONDS" -lt "$deadline" ]]; do
  if rg -q 'NATIVE_KOBOX2_IPC=(PASS|FAIL)|ACTION=terminate|PANIC|init ELF load failed' "$out/serial.log"; then break; fi
  sleep 0.2
done
stop_qemu
trap - EXIT
if ! rg -q 'NATIVE_KOBOX2_IPC=PASS' "$out/serial.log" || \
   ! rg -q 'NATIVE_KOBOX2_IPC_TIMED=PASS' "$out/serial.log" || \
   ! rg -q 'NATIVE_GPUD_PROCESS=PASS' "$out/serial.log" || \
   ! rg -q 'NATIVE_GPUD_LAUNCH=PASS' "$out/serial.log" || \
   ! rg -q 'NATIVE_GPUD_LAUNCH_ABORT=PASS' "$out/serial.log" || \
   ! rg -q 'NATIVE_GPUD_CONTROLLER_LAUNCH=PASS' "$out/serial.log" || \
   ! rg -q 'NATIVE_KOBOX2_IPC_BACKPRESSURE=PASS' "$out/serial.log" || \
   ! rg -q 'NATIVE_KOBOX2_IPC_STALE=PASS' "$out/serial.log" || \
   ! rg -q 'NATIVE_KOBOX2_BOOTSTRAP=PASS' "$out/serial.log" || \
   ! rg -q 'NATIVE_KOBOX2_BOOTSTRAP_REJECT=PASS' "$out/serial.log" || \
   ! rg -q 'NATIVE_KOBOX2_PACKAGE_SNAPSHOT=PASS' "$out/serial.log" || \
   ! rg -q 'NATIVE_KOBOX2_PACKAGE_CORRUPT=PASS' "$out/serial.log" || \
   ! rg -q 'NATIVE_KOBOX2_PACKAGE_STALE=PASS' "$out/serial.log" || \
   [[ "$(rg -c 'NATIVE_KOBOX2_IPC_ROUND=PASS' "$out/serial.log")" != 2 ]]; then
  tail -100 "$out/serial.log"
  exit 1
fi
if rg -n 'NATIVE_KOBOX2_IPC=FAIL|PANIC|ACTION=terminate' "$out/serial.log"; then exit 1; fi
printf 'Native kobox2 IPC passed: %s\n' "$out/serial.log"
