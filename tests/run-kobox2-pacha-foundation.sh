#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/kobox2-pacha-foundation"
gates=(PACHA_KOBOX_NATIVE_MAPPING PACHA_KOBOX_BOOT_SMP PACHA_KOBOX_MEMORY PACHA_KOBOX_WAIT
  PACHA_KOBOX_RCU PACHA_KOBOX_WORKQUEUE PACHA_KOBOX_CLEANUP PACHA_KOBOX_FOUNDATION)
terminal='PACHA_KOBOX_FOUNDATION=(PASS|FAIL)'
init="$out/kobox2_foundation.elf"
manifest="$repo_root/tests/kobox2-foundation.bootfs"
core="$repo_root/.artifacts/kobox2-separated-gate-runtime/linux-boot-runtime.so"
qemu_devices=()
if [[ "${1:-}" == --native-primitives ]]; then
  [[ -z "${KOBOX_PACHA_MANIFEST:-}" ]] || { echo "No core in native mode" >&2; exit 2; }
  out="$repo_root/.artifacts/tests/native-host-primitives/boot-only"
  mkdir -p "$out"
  bash "$repo_root/tests/build-native-host-primitives.sh" --init
  init="$repo_root/.artifacts/tests/native-host-primitives/native_host_primitives_init.elf"
  gates=(NATIVE_PENDING_NOTIFICATION_RETURN NATIVE_THREAD_NOTIFICATION NATIVE_FAULT_RETURN NATIVE_EXECUTION NATIVE_CLOCK_CONTRACT)
  terminal='NATIVE_CLOCK_CONTRACT=PASS|NATIVE_[A-Z_]+=FAIL'
else
  if [[ $# == 1 && "$1" == --device ]]; then
    out="$repo_root/.artifacts/tests/kobox2-pacha-device"
    init="$out/kobox2_foundation.elf"
    # Make the selected manifest explicit to both the builder and the runner.
    export KOBOX_PACHA_MANIFEST="${KOBOX_PACHA_MANIFEST:-$repo_root/tests/kobox2-device.bootfs}"
    gates+=(PACHA_KOBOX_VFS PACHA_KOBOX_SHMEM PACHA_KOBOX_PRESSURE
      PACHA_KOBOX_ALLOCATION PACHA_KOBOX_CLIENT_TASK PACHA_KOBOX_CHAPTER2_CORE PACHA_KOBOX_DEVICE)
    qemu_devices=(-device virtio-gpu-pci,disable-legacy=on,iommu_platform=on)
  elif [[ $# == 1 && "$1" == --vfs-stress ]]; then
    out="$repo_root/.artifacts/tests/kobox2-pacha-vfs-stress"
    init="$out/kobox2_foundation.elf"
    # Diagnostic workload only: never claim the full chapter-2 Gate here.
    gates+=(PACHA_KOBOX_VFS_STRESS)
  elif [[ $# == 1 && ( "$1" == --chapter2-core || "$1" == --vm* || "$1" == --syscall* || "$1" == --exec* || "$1" == --gem* ) ]]; then
    if [[ "$1" == --vm* || "$1" == --syscall* || "$1" == --exec* || "$1" == --gem* ]]; then
      out="$repo_root/.artifacts/tests/kobox2-pacha-vm"
      manifest="$repo_root/tests/kobox2-vm.bootfs"
      gates+=(PACHA_KOBOX_VM_NATIVE_PROTECTION)
      if [[ "$1" == --gem || "$1" == --gem-focused ]]; then
        out="$repo_root/.artifacts/tests/kobox2-pacha-gem"
        gates+=(PACHA_KOBOX_GEM)
      elif [[ "$1" == --exec || "$1" == --exec-focused ]]; then
        out="$repo_root/.artifacts/tests/kobox2-pacha-exec"
        manifest="$repo_root/tests/kobox2-exec.bootfs"
        gates+=(PACHA_KOBOX_EXEC)
      elif [[ "$1" == --syscall || "$1" == --syscall-focused ]]; then
        out="$repo_root/.artifacts/tests/kobox2-pacha-syscall"
        gates+=(PACHA_KOBOX_SYSCALL PACHA_KOBOX_SYSCALL_CONTEXT)
      else
        gates+=(PACHA_KOBOX_VM)
      fi
    else
      out="$repo_root/.artifacts/tests/kobox2-pacha-chapter2-core"
    fi
    init="$out/kobox2_foundation.elf"
    gates+=(PACHA_KOBOX_VFS PACHA_KOBOX_SHMEM PACHA_KOBOX_PRESSURE
      PACHA_KOBOX_ALLOCATION PACHA_KOBOX_CLIENT_TASK PACHA_KOBOX_CHAPTER2_CORE)
    if [[ "$1" == --vm-focused || "$1" == --syscall-focused || "$1" == --exec-focused || "$1" == --gem-focused ]]; then
      out+="-focused"
      init="$out/kobox2_foundation.elf"
      gates=(PACHA_KOBOX_NATIVE_MAPPING PACHA_KOBOX_BOOT_SMP
        PACHA_KOBOX_VM_NATIVE_PROTECTION PACHA_KOBOX_FOCUSED)
      if [[ "$1" == --gem-focused ]]; then
        gates+=(PACHA_KOBOX_GEM)
      elif [[ "$1" == --exec-focused ]]; then
        gates+=(PACHA_KOBOX_EXEC)
      elif [[ "$1" == --syscall-focused ]]; then
        gates+=(PACHA_KOBOX_SYSCALL PACHA_KOBOX_SYSCALL_CONTEXT)
      else
        gates+=(PACHA_KOBOX_VM)
      fi
      terminal='PACHA_KOBOX_FOCUSED=PASS|PACHA_KOBOX_FOUNDATION=FAIL'
    fi
  else
    [[ $# == 0 ]] || { echo "Unknown argument: $1" >&2; exit 2; }
  fi
  if [[ -n "${KOBOX_PACHA_MANIFEST:-}" ]]; then
    manifest="$(realpath "$KOBOX_PACHA_MANIFEST")"
    out+="-diagnostic"
    init="$out/kobox2_foundation.elf"
  fi
  # These test manifests use explicit destination=source entries. Resolve the
  # one core entry before building; an override must never inspect one ELF
  # and silently boot the canonical ELF instead.
  mapfile -t core_entries < <(sed -n 's|^/srv/kobox2/core.so=||p' "$manifest")
  [[ "${#core_entries[@]}" == 1 ]] || { echo "Expected one core entry" >&2; exit 2; }
  core="${core_entries[0]}"
  if [[ "$core" != /* ]]; then
    core="$(dirname "$manifest")/$core"
  fi
  core="$(realpath -e "$core")"
  KOBOX_PACHA_CORE="$core" bash "$repo_root/tests/build-kobox2-pacha-foundation.sh" "$@"
fi
(cd "$repo_root/kernel" && zig build limine)
(cd "$repo_root/pack" && go run ./cmd/bootfs-image \
  --manifest "$manifest" --output "$out/bootfs.img")
sha256sum "$core" \
  "$repo_root/kernel/zig-out/bin/limine/pacha-kernel.elf" "$init" "$out/bootfs.img" \
  >"$out/inputs.sha256"
if [[ "${1:-}" == --vm* || "${1:-}" == --syscall* || "${1:-}" == --exec* || "${1:-}" == --gem* ]]; then
  sha256sum "$repo_root/.artifacts/tests/kobox2-pacha-vm/vm_client.elf" \
    >>"$out/inputs.sha256"
fi
if [[ "${1:-}" == --exec* ]]; then
  sha256sum "$repo_root/.artifacts/tests/kobox2-pacha-exec/linux_client.elf" \
    >>"$out/inputs.sha256"
fi
if [[ "${1:-}" == --gem* || "${1:-}" == --device ]]; then
  sha256sum "$manifest" >>"$out/inputs.sha256"
  # The build validated the bytes selected by this exact manifest.
  sed -n 'p' "$out/module-inputs.sha256" >>"$out/inputs.sha256"
fi
# Only the 64 MiB bootloader image is cloned. No rootfs disk is attached.
# The native sandbox is the test init process. Only --device consumes its
# dedicated virtio-gpu grant; the foundation/VM fixtures consume none.
cp --reflink=auto "$repo_root/.artifacts/limine-boot.img" "$out/boot.img"
mcopy -o -i "$out/boot.img@@2097152" "$repo_root/kernel/zig-out/bin/limine/pacha-kernel.elf" ::/KERNEL.ELF
mcopy -o -i "$out/boot.img@@2097152" "$init" ::/INITAPP.ELF
mcopy -o -i "$out/boot.img@@2097152" "$out/bootfs.img" ::/BOOTFS.IMG
: >"$out/serial.log"
qemu-system-x86_64 \
  -machine q35 -cpu host -enable-kvm -m 2G -smp 2 \
  -device intel-iommu,intremap=off,aw-bits=48 \
  "${qemu_devices[@]}" \
  -drive "file=$out/boot.img,format=raw,if=ide" \
  -display none -serial "file:$out/serial.log" -monitor none -no-reboot \
  >"$out/qemu.log" 2>&1 &
qemu_pid=$!
stop_qemu() {
  if kill -0 "$qemu_pid" 2>/dev/null; then kill "$qemu_pid"; fi
  wait "$qemu_pid" || true
}
trap stop_qemu EXIT
deadline=$((SECONDS + ${KOBOX_PACHA_TIMEOUT:-180}))
while kill -0 "$qemu_pid" 2>/dev/null; do
  if [[ -f "$out/serial.log" ]] && rg -q \
    "$terminal|ACTION=terminate|init ELF load failed" "$out/serial.log"; then
    break
  fi
  [[ "$SECONDS" -lt "$deadline" ]] || break
  sleep 0.2
done
stop_qemu
trap - EXIT
for gate in "${gates[@]}"; do
  rg -q "${gate}=PASS" "$out/serial.log" || {
    tail -100 "$out/serial.log"; exit 1;
  }
done
if rg -n 'PACHA_KOBOX_FOUNDATION=FAIL|NATIVE_[A-Z_]+=FAIL|Kernel panic|PANIC|ACTION=terminate' "$out/serial.log"; then
  exit 1
fi
printf 'PachaOS Gates passed: %s\n' "$out/serial.log"
