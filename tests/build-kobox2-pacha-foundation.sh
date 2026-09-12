#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
adapter="$repo_root/userland/kobox2_adapter"
sandbox="$repo_root/kobox2/linux-sandbox/kobox"
out="$repo_root/.artifacts/tests/kobox2-pacha-foundation"
extra_flags=()
extra_sources=()
chapter2_source="$adapter/chapter2.c"
case "${1:-}" in
  "") [[ $# == 0 ]] || exit 2 ;;
  --vfs-stress)
    [[ $# == 1 ]] || exit 2
    out="$repo_root/.artifacts/tests/kobox2-pacha-vfs-stress"
    extra_flags+=(-DPH_CHAPTER2_CORE=1)
    chapter2_source="$repo_root/tests/kobox2_vfs_reclaim_stress.c"
    ;;
  --chapter2-core)
    [[ $# == 1 ]] || exit 2
    out="$repo_root/.artifacts/tests/kobox2-pacha-chapter2-core"
    extra_flags+=(-DPH_CHAPTER2_CORE=1)
    ;;
  --device)
    [[ $# == 1 && -n "${KOBOX_PACHA_MANIFEST:-}" ]] || exit 2
    out="$repo_root/.artifacts/tests/kobox2-pacha-device"
    extra_flags+=(-DPH_CHAPTER2_CORE=1 -DPH_DEVICE_GATE=1)
    extra_sources+=("$adapter/device.c" "$adapter/device_gate.c" "$adapter/device_pci.c"
      "$adapter/device_dma.c" "$adapter/device_irq.c" "$adapter/device_irq_queue.c")
    ;;
  --vm|--syscall|--vm-focused|--syscall-focused|--exec|--exec-focused|--gem|--gem-focused)
    [[ $# == 1 ]] || exit 2
    out="$repo_root/.artifacts/tests/kobox2-pacha-vm"
    vm_cases=(basic readonly reuse irq truncate late-fault pressure-fault
      exit-publish exit lifetime death rollback)
    selected_case=-1
    for index in "${!vm_cases[@]}"; do
      if [[ "${vm_cases[index]}" == "${KOBOX_PACHA_VM_CASE:-basic}" ]]; then
        selected_case="$index"
      fi
    done
    [[ "$selected_case" -ge 0 ]] || { echo "Unknown VM case" >&2; exit 2; }
    extra_flags+=(-DPH_CHAPTER2_CORE=1 -DPH_VM_GATE=1 -DPH_VM_CASE="$selected_case")
    if [[ "$1" == --syscall || "$1" == --syscall-focused ]]; then
      out="$repo_root/.artifacts/tests/kobox2-pacha-syscall"
      case "${KOBOX_PACHA_SYSCALL_CASE:-base}" in
        base) extra_flags+=(-DPH_SYSCALL_GATE=0) ;;
        rights) extra_flags+=(-DPH_SYSCALL_GATE=1) ;;
        rights-race) extra_flags+=(-DPH_SYSCALL_GATE=2) ;;
        inheritance) extra_flags+=(-DPH_SYSCALL_GATE=3) ;;
        fork) extra_flags+=(-DPH_SYSCALL_GATE=4) ;;
        clone) extra_flags+=(-DPH_SYSCALL_GATE=5) ;;
        thread) extra_flags+=(-DPH_SYSCALL_GATE=6) ;;
        thread-exit-wait) extra_flags+=(-DPH_SYSCALL_GATE=7) ;;
        thread-exit-running) extra_flags+=(-DPH_SYSCALL_GATE=8) ;;
        thread-exit-peer) extra_flags+=(-DPH_SYSCALL_GATE=9) ;;
        client-run) extra_flags+=(-DPH_SYSCALL_GATE=10) ;;
        *) echo "Unknown syscall case" >&2; exit 2 ;;
      esac
    fi
    if [[ "$1" == --exec || "$1" == --exec-focused ]]; then
      out="$repo_root/.artifacts/tests/kobox2-pacha-exec"
      extra_flags+=(-DPH_EXEC_GATE=1)
      if [[ -n "${KOBOX_PACHA_EXEC_ENTRY:-}" ]]; then
        echo "KOBOX_PACHA_EXEC_ENTRY was removed; exec uses the CALL entry" >&2
        exit 2
      fi
      bash "$repo_root/tests/build-kobox2-pacha-exec.sh"
      extra_sources+=("$adapter/exec_image.c" "$adapter/exec_fault.c" "$adapter/exec_gate.c" "$out/Decoder.o" "$out/DecoderData.o"
        "$out/SharedData.o" "$out/patch_scan.o")
    fi
    if [[ "$1" == --gem || "$1" == --gem-focused ]]; then
      [[ -n "${KOBOX_PACHA_MANIFEST:-}" ]] || {
        echo "GEM requires an explicit core/module bootfs manifest" >&2; exit 2;
      }
      out="$repo_root/.artifacts/tests/kobox2-pacha-gem"
      extra_flags+=(-DPH_GEM_GATE=1)
      extra_sources+=("$adapter/gem_gate.c" "$adapter/module_access.c" "$adapter/module_access_entry.S")
    fi
    if [[ "$1" == *-focused ]]; then
      out+="-focused"
      extra_flags+=(-DPH_FOCUSED_GATE=1)
    fi
    extra_sources+=("$adapter/vm.c" "$adapter/vm_process.c" "$adapter/vm_syscall.c"
      "$adapter/vm_context.c" "$adapter/vm_execution.c" "$adapter/vm_lpr.c"
      "$adapter/vm_memory.c" "$adapter/vm_clone.c"
      "$repo_root/userland/personality/linux/runtime/lpr_zpoline.c" "$adapter/vm_gate.c")
    bash "$repo_root/tests/build-kobox2-pacha-vm-client.sh"
    ;;
  *) echo "Unknown argument: $1" >&2; exit 2 ;;
esac
# The runner resolves this from its manifest: inspect the exact image booted.
core="${KOBOX_PACHA_CORE:-$repo_root/.artifacts/kobox2-separated-gate-runtime/linux-boot-runtime.so}"
if [[ -n "${KOBOX_PACHA_MANIFEST:-}" ]]; then
  out+="-diagnostic"
fi
mkdir -p "$out"
if [[ "${1:-}" == --gem* ]]; then
  python3 "$repo_root/tests/check-kobox2-pacha-modules.py" \
    --manifest "$KOBOX_PACHA_MANIFEST" \
    --inputs "${KOBOX_PACHA_GEM_INPUTS:-$repo_root/.artifacts/kobox2-pacha-chapter2-modules/gem-module-inputs.json}" \
    >"$out/module-inputs.sha256"
fi
if [[ "${1:-}" == --device ]]; then
  python3 "$repo_root/tests/check-kobox2-pacha-modules.py" --profile virtio \
    --manifest "$KOBOX_PACHA_MANIFEST" \
    --inputs "${KOBOX_PACHA_DEVICE_INPUTS:-$repo_root/.artifacts/kobox2-pacha-device-modules/gem-module-inputs.json}" \
    >"$out/module-inputs.sha256"
fi
python3 "$sandbox/boot/inspect_core.py" --core "$core" \
  --inputs "$(dirname "$core")/linux-boot-inputs.json"
adapter_sources=(
  "$adapter/runtime.c"
  "$adapter/native.c"
  "$adapter/machine.c"
  "$adapter/image.c"
  "$adapter/exception.c"
  "$adapter/bootfs.c"
  "$adapter/foundation.c"
  "$adapter/boot.c"
  "$adapter/foundation_bootfs.c"
  "$chapter2_source"
  "$adapter/entry.S"
)
"${CAPOS_FREESTANDING_CC:-clang}" -target x86_64-linux-gnu \
  -fuse-ld=lld -nostdlib -static-pie -ffreestanding -fno-builtin \
  -fno-stack-protector -fPIE -mno-red-zone -std=c11 -O2 -g \
  -DPH_CLEANUP_ROUNDS="${KOBOX_PACHA_CLEANUP_ROUNDS:-1}" \
  "${extra_flags[@]}" \
  -Wall -Wextra -Werror -I "$repo_root/userland/libpacha/include" \
  -I "$repo_root/userland/libipc/include" -I "$sandbox" -I "$adapter" \
  -I "$repo_root/userland/libcapsule/include" \
  -I "$repo_root/userland/personality/include" \
  -I "$repo_root/userland/personality/linux/decoder" \
  "${adapter_sources[@]}" \
  "${extra_sources[@]}" \
  "$repo_root/tests/kobox2_native_mapping.c" \
  "$repo_root/userland/libpacha/src/syscall.c" \
  "$sandbox/machine/domain.c" "$sandbox/boot/core.c" \
  -Wl,-e,_start,-z,noexecstack \
  -o "$out/kobox2_foundation.elf"
sha256sum "$core" "$out/kobox2_foundation.elf"
