#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/native-gpud-core"
device_flags=()
manifest="$repo_root/tests/kobox2-gpud-core.bootfs"
case "${1:-}" in
  "") [[ $# == 0 ]] || exit 2 ;;
  --device)
    [[ $# == 1 ]] || exit 2
    out="$repo_root/.artifacts/tests/native-gpud-device"
    manifest="$repo_root/tests/kobox2-gpud-device.bootfs"
    device_flags=(-DGPUD_CORE_TEST_DEVICE=1)
    ;;
  *) exit 2 ;;
esac
adapter="$repo_root/userland/kobox2_adapter"
gpud="$repo_root/userland/gpud"
sandbox="$repo_root/kobox2/linux-sandbox/kobox"
protocol="$repo_root/kobox2/protocol"
compiler="${CAPOS_FREESTANDING_CC:-clang}"
core="${KOBOX_NATIVE_CORE:-$repo_root/.artifacts/kobox2-device-launch-runtime/linux-boot-runtime.so}"
mkdir -p "$out/musl"
python3 "$sandbox/boot/inspect_core.py" --core "$core" \
  --inputs "$(dirname "$core")/linux-boot-inputs.json"
cp --reflink=auto "$core" "$out/package-core.so"
sha256sum "$core" \
  "$repo_root/.artifacts/kobox2-device-launch-modules/drivers/gpu/drm/drm_panel_orientation_quirks.ko" \
  >"$out/package-inputs.sha256"
# Only native musl's sorting objects, no runtime installation or rootfs copy.
make -C "$out/musl" -f "$repo_root/musl/upstream/Makefile" \
  "srcdir=$repo_root/musl/upstream" ARCH=pachaos "CC=$compiler" \
  'CFLAGS=-target x86_64-linux-musl -mno-red-zone -fno-stack-protector -fPIE -O2' \
  obj/src/stdlib/qsort.o obj/src/stdlib/qsort_nr.o
common=(-target x86_64-linux-gnu -fuse-ld=lld -nostdlib -static-pie -ffreestanding
  -fno-builtin -fno-stack-protector -fPIE -mno-red-zone -O2 -g -Wall -Wextra -Werror
  -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/libipc/include"
  -I "$protocol/include" -I "$protocol/generated/include")
shared=("$adapter/ipc.c" "$adapter/bootstrap.c" "$repo_root/userland/libpacha/src/syscall.c"
  "$protocol/src/closure_manifest.c" "$protocol/src/resource_grant.c" "$protocol/src/sha256.c")
sandbox_flags=(--foundation-gates)
[[ "${1:-}" == --device ]] || sandbox_flags+=(--no-device)
bash "$adapter/build-sandbox.sh" "$out/child.elf" "${sandbox_flags[@]}"
parent=("$repo_root/tests/native_gpud_core.c" "$repo_root/tests/native_test_init.c"
  "$repo_root/userland/seed0boot/src/bootfs_reader.c"
  "$gpud/launch_native.c" "$gpud/arch/x86_64/launch_image.c" "$gpud/process_native.c"
  "$gpud/launch.c" "$gpud/bootstrap.c" "$gpud/process.c" "$gpud/lifecycle.c"
  "$repo_root/kobox2/src/controller/controller.c" "$repo_root/kobox2/src/closure.c"
  "$out/musl/obj/src/stdlib/qsort.o" "$out/musl/obj/src/stdlib/qsort_nr.o")
if [[ "${1:-}" == --device ]]; then
  parent+=("$gpud/drm_translate.c" "$gpud/drm_reply.c" "$protocol/src/gpu.c"
    "$gpud/gpu_rpc.c"
    "$protocol/src/gpu_session.c"
    "$gpud/gpu_channel.c" "$protocol/src/virtqueue.c" "$protocol/src/virtqueue_memory.c"
    "$protocol/arch/x86_64/virtqueue_atomic.c"
    "$protocol/src/gpu_completion.c" "$protocol/src/protocol.c")
fi
if [[ "${1:-}" == --device ]]; then
  python3 "$repo_root/tests/check-kobox2-pacha-modules.py" --profile virtio \
    --manifest "$manifest" \
    --inputs "$repo_root/.artifacts/kobox2-device-launch-modules/gem-module-inputs.json" \
    >>"$out/package-inputs.sha256"
fi
"$compiler" "${common[@]}" "${device_flags[@]}" -I "$repo_root/kobox2/include" \
  -I "$repo_root/userland/gpud/include" \
  -I "$repo_root/userland/libcapsule/include" -DSEED0_BOOTFS_NO_DIAGNOSTICS \
  "${parent[@]}" "${shared[@]}" -Wl,-e,_start,-z,noexecstack -o "$out/parent.elf"
# Keep transitive project headers as well as the directly compiled files.
mapfile -t headers < <(rg --files "$adapter" "$gpud" "$protocol" "$sandbox/boot" \
  "$sandbox/arch" "$sandbox/machine" "$repo_root/kobox2/include" \
  "$repo_root/userland/libpacha/include" "$repo_root/userland/libipc/include" -g '*.h' | sort)
sha256sum "$out/child.elf.sources.sha256" "${parent[@]}" "${shared[@]}" "${headers[@]}" \
  "$repo_root/tests/native_gpud_core_common.h" "$repo_root/tests/native_kobox2_ipc_common.h" \
  "$repo_root/tests/build-native-gpud-core.sh" "$repo_root/tests/run-native-gpud-core.sh" \
  "$manifest" >"$out/source-inputs.sha256"
