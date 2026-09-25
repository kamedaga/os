#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
adapter="$repo_root/userland/kobox2_adapter"
gpud="$repo_root/userland/gpud"
sandbox="$repo_root/kobox2/linux-sandbox/kobox"
protocol="$repo_root/kobox2/protocol"
out="${1:-$repo_root/.artifacts/gpud/sandbox.elf}"
[[ $# == 0 ]] || shift
gates=0
device=1
dma_profile=0
usb_hid=0
net=0
for argument in "$@"; do
  case "$argument" in
    --foundation-gates) gates=1 ;;
    --no-device) device=0 ;;
    --dma-profile) dma_profile=1 ;;
    --usb-hid) usb_hid=1 ;;
    --virtio-net|--net) net=1 ;;
    *) echo "Unknown sandbox build argument: $argument" >&2; exit 2 ;;
  esac
done
mkdir -p "$(dirname "$out")"
sources=("$adapter/sandbox_main.c" "$adapter/runtime.c" "$adapter/entry.S"
  "$adapter/native.c" "$adapter/machine.c" "$adapter/image.c" "$adapter/exception.c"
  "$adapter/boot.c" "$adapter/package.c" "$adapter/module_package.c"
  "$adapter/lifecycle.c" "$adapter/ipc.c" "$adapter/bootstrap.c"
  "$repo_root/userland/libpacha/src/syscall.c"
  "$sandbox/boot/package.c" "$sandbox/boot/fixed_image.c" "$sandbox/arch/x86_64/elf.c"
  "$sandbox/machine/domain.c" "$sandbox/boot/core.c"
  "$protocol/src/closure_manifest.c" "$protocol/src/resource_grant.c" "$protocol/src/sha256.c")
if [[ "$usb_hid" == 1 && "$net" == 1 ]]; then
  echo 'select one sandbox device profile' >&2; exit 2
fi
if [[ ( "$usb_hid" == 1 || "$net" == 1 ) && "$device" != 1 ]]; then
  echo 'hosted PCI profile requires a granted PCI device' >&2; exit 2
fi
flags=(-DPH_SANDBOX_DEVICE="$device" -DPH_SANDBOX_USB_HID="$usb_hid"
  -DPH_SANDBOX_NET="$net")
if [[ "$dma_profile" == 1 ]]; then
  flags+=(-DPH_DMA_PROFILE=1)
fi
if [[ "$gates" == 1 ]]; then
  flags+=(-DPH_SANDBOX_FOUNDATION_GATES=1 -DPH_CHAPTER2_CORE=1)
  sources+=("$adapter/foundation.c" "$adapter/chapter2.c" "$repo_root/tests/kobox2_native_mapping.c")
fi
if [[ "$device" == 1 ]]; then
  sources+=("$adapter/device_grant.c" "$adapter/device.c" "$adapter/device_pci.c"
    "$adapter/device_dma.c" "$adapter/device_irq.c" "$adapter/device_irq_queue.c")
  if [[ "$usb_hid" == 1 ]]; then
    sources+=("$adapter/usb_input_service.c")
  elif [[ "$net" == 1 ]]; then
    sources+=("$adapter/net_frame_service.c")
  else
    sources+=("$adapter/gpu_query.c" "$sandbox/boot/drm_query.c"
      "$adapter/gpu_session_service.c" "$gpud/gpu_sessions.c"
      "$protocol/src/gpu_session.c" "$adapter/gpu_queue.c"
      "$gpud/gpu_channel.c" "$protocol/src/virtqueue.c"
      "$protocol/src/virtqueue_memory.c"
      "$protocol/arch/x86_64/virtqueue_atomic.c" "$protocol/src/gpu.c"
      "$protocol/src/gpu_completion.c" "$protocol/src/protocol.c")
  fi
fi
"${CAPOS_FREESTANDING_CC:-clang}" -target x86_64-linux-gnu -fuse-ld=lld -nostdlib -static-pie \
  -ffreestanding -fno-builtin -fno-stack-protector -fPIE -mno-red-zone -std=c11 -O2 -g \
  -Wall -Wextra -Werror "${flags[@]}" -I "$sandbox" -I "$adapter" \
  -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/libipc/include" \
  -I "$repo_root/userland/libcapsule/include" -I "$repo_root/userland/personality/include" \
  -I "$protocol/include" -I "$protocol/generated/include" "${sources[@]}" \
  -Wl,-e,_start,-z,noexecstack -o "$out"
if nm "$out" | rg ' [Tt] (kb2_controller_|kb2_closure_builder_|gpud_)'; then
  echo 'Controller code leaked into the GPL sandbox' >&2; exit 1
fi
mapfile -t headers < <(rg --files "$adapter" "$gpud" "$protocol" "$sandbox/boot" \
  "$sandbox/arch" "$sandbox/machine" "$repo_root/userland/libpacha/include" \
  "$repo_root/userland/libipc/include" -g '*.h' | sort)
sha256sum "${sources[@]}" "${headers[@]}" "$0" >"$out.sources.sha256"
