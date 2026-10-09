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
scheduler_diagnostic=0
clock_timespec_bench=0
usb_hid=0
net=0
amdgpu=0
block=0
filesystem=0
fs_profile=0
fs_owner_publish=0
cache_preflight=0
no_clock_page=0
tail_trace=0
for argument in "$@"; do
  case "$argument" in
    --foundation-gates) gates=1 ;;
    --no-device) device=0 ;;
    --dma-profile) dma_profile=1 ;;
    --scheduler-diagnostic) scheduler_diagnostic=1 ;;
    --clock-timespec-bench) clock_timespec_bench=1 ;;
    --usb-hid) usb_hid=1 ;;
    --usb-storage) usb_hid=1; block=1 ;;
    --nvme) block=1 ;;
    --storage) filesystem=1 ;;
    --fs-profile) fs_profile=1 ;;
    --fs-owner-publish) fs_owner_publish=1 ;;
    --virtio-net|--net) net=1 ;;
    --amdgpu) amdgpu=1 ;;
    --cache-preflight) cache_preflight=1 ;;
    --no-clock-page) no_clock_page=1 ;;
    --tail-trace) tail_trace=1 ;;
    *) echo "Unknown sandbox build argument: $argument" >&2; exit 2 ;;
  esac
done
mkdir -p "$(dirname "$out")"
sources=("$adapter/sandbox_main.c" "$adapter/runtime.c" "$adapter/entry.S"
  "$adapter/native.c" "$adapter/fiber.S" "$adapter/machine.c" "$adapter/image.c" "$adapter/exception.c"
  "$adapter/boot.c" "$adapter/package.c" "$adapter/module_package.c"
  "$adapter/lifecycle.c" "$adapter/ipc.c" "$adapter/bootstrap.c"
  "$repo_root/userland/libpacha/src/syscall.c"
  "$sandbox/boot/package.c" "$sandbox/boot/fixed_image.c" "$sandbox/arch/x86_64/elf.c"
  "$sandbox/machine/domain.c" "$sandbox/boot/core.c"
  "$protocol/src/closure_manifest.c" "$protocol/src/resource_grant.c" "$protocol/src/sha256.c")
if (( usb_hid + net + amdgpu > 1 )); then
  echo 'select one sandbox device profile' >&2; exit 2
fi
if (( block == 1 && (net == 1 || amdgpu == 1) )); then
  echo 'block service needs USB or NVMe PCI profile' >&2; exit 2
fi
if (( filesystem == 1 && (block == 1 || net == 1 || amdgpu == 1 || usb_hid == 1 || device == 0) )); then
  echo 'filesystem service requires its storage PCI profile' >&2; exit 2
fi
if [[ ( "$usb_hid" == 1 || "$net" == 1 || "$amdgpu" == 1 || "$block" == 1 ) && "$device" != 1 ]]; then
  echo 'hosted PCI profile requires a granted PCI device' >&2; exit 2
fi
if [[ "$cache_preflight" == 1 && "$amdgpu" != 1 ]]; then
  echo 'cache preflight requires the AMDGPU sandbox log profile' >&2; exit 2
fi
flags=(-DPH_SANDBOX_DEVICE="$device" -DPH_SANDBOX_USB_HID="$usb_hid"
  -DPH_SANDBOX_NET="$net" -DPH_SANDBOX_AMDGPU="$amdgpu"
  -DPH_SANDBOX_BLOCK="$block"
  -DPH_SANDBOX_FS="$filesystem"
  -DPH_CACHE_PREFLIGHT="$cache_preflight")
if [[ "$dma_profile" == 1 ]]; then
  flags+=(-DPH_DMA_PROFILE=1)
fi
if [[ "$scheduler_diagnostic" == 1 ]]; then
  flags+=(-DPH_SCHED_DIAGNOSTIC=1)
fi
if [[ "$tail_trace" == 1 ]]; then
  if [[ "$filesystem" != 1 ]]; then echo '--tail-trace requires --storage' >&2; exit 2; fi
  flags+=(-DPH_TAIL_TRACE=1)
  sources+=("$adapter/tail_trace.c")
fi
if [[ "$fs_profile" == 1 ]]; then
  if [[ "$filesystem" != 1 ]]; then echo '--fs-profile requires --storage' >&2; exit 2; fi
  flags+=(-DPH_FS_STORAGE_PROFILE=1)
fi
if [[ "$fs_owner_publish" == 1 ]]; then
  if [[ "$filesystem" != 1 ]]; then echo '--fs-owner-publish requires --storage' >&2; exit 2; fi
  # Controlled comparison with the same core/header; never a wire fallback.
  flags+=(-DPH_FS_OWNER_PUBLISH=1)
fi
if [[ "$clock_timespec_bench" == 1 ]]; then
  flags+=(-DPH_CLOCK_TIMESPEC_BENCH=1)
fi
# Comparison control only: keep every monotonic read on CLOCK_READ.
if [[ "$no_clock_page" == 1 ]]; then
  flags+=(-DPH_CLOCK_PAGE_DISABLED=1)
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
    if [[ "$block" == 1 ]]; then
      sources+=("$adapter/usb_storage_service.c")
    fi
  elif [[ "$net" == 1 ]]; then
    sources+=("$adapter/net_frame_service.c")
  elif [[ "$amdgpu" == 1 ]]; then
    sources+=("$adapter/firmware_bundle.c" "$repo_root/userland/libpacha/src/log.c")
  elif [[ "$block" == 1 ]]; then
    :
  elif [[ "$filesystem" == 1 ]]; then
    sources+=("$adapter/fs_queue.c" "$repo_root/userland/kobox2_owner/fs_channel.c"
      "$protocol/src/filesystem.c" "$protocol/src/virtqueue.c"
      "$protocol/src/virtqueue_memory.c" "$protocol/arch/x86_64/virtqueue_atomic.c"
      "$protocol/src/protocol.c")
  else
    sources+=("$adapter/gpu_query.c" "$sandbox/boot/drm_query.c"
      "$adapter/gpu_session_service.c" "$gpud/gpu_sessions.c"
      "$protocol/src/gpu_session.c" "$adapter/gpu_queue.c"
      "$gpud/gpu_channel.c" "$protocol/src/virtqueue.c"
      "$protocol/src/virtqueue_memory.c"
      "$protocol/arch/x86_64/virtqueue_atomic.c" "$protocol/src/gpu.c"
      "$protocol/src/gpu_completion.c" "$protocol/src/protocol.c")
  fi
  if [[ "$block" == 1 ]]; then
    sources+=("$adapter/block_service.c" "$protocol/src/block_device.c")
  fi
  if [[ "$amdgpu" == 1 ]]; then
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
# The direct notification ABI relies on the entire fast-path callgraph being
# GPR-only, including compiler prologues and the current-task helper.
python3 "$repo_root/tests/check_kobox2_notification_fp.py" "$out" \
  > "$out.notification-fp.jsonl"
mapfile -t headers < <(rg --files "$adapter" "$gpud" "$protocol" "$sandbox/boot" \
  "$sandbox/arch" "$sandbox/machine" "$repo_root/userland/libpacha/include" \
  "$repo_root/userland/libipc/include" -g '*.h' | sort)
headers+=("$sandbox/task/host.h")
sha256sum "${sources[@]}" "${headers[@]}" "$0" >"$out.sources.sha256"
