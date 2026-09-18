#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/native-kobox2-ipc"
mkdir -p "$out"
compiler="${CAPOS_FREESTANDING_CC:-clang}"
# The native controller fixture needs libc sorting, not a second sorter in
# gpud. Build just musl's two qsort objects in this fixture's output tree.
# This does not rebuild/install musl runtime or copy/synchronize any rootfs.
mkdir -p "$out/musl"
make -C "$out/musl" -f "$repo_root/musl/upstream/Makefile" \
  "srcdir=$repo_root/musl/upstream" ARCH=pachaos "CC=$compiler" \
  'CFLAGS=-target x86_64-linux-musl -mno-red-zone -fno-stack-protector -fPIE -O2' \
  obj/src/stdlib/qsort.o obj/src/stdlib/qsort_nr.o
sandbox="$repo_root/kobox2/linux-sandbox/kobox"
protocol="$repo_root/kobox2/protocol"
core="$repo_root/.artifacts/gpud-production-runtime/linux-boot-runtime.so"
module="$repo_root/.artifacts/kobox2-pacha-device-modules/drivers/gpu/drm/drm_panel_orientation_quirks.ko"
sha256sum "$core" "$module" >"$out/package-inputs.sha256"
python3 "$repo_root/tests/check-kobox2-pacha-modules.py" --profile virtio \
  --manifest "$repo_root/tests/kobox2-device.bootfs" \
  --inputs "$repo_root/.artifacts/kobox2-pacha-device-modules/gem-module-inputs.json" \
  >"$out/module-inputs.sha256"
common=(-target x86_64-linux-gnu -fuse-ld=lld -nostdlib -ffreestanding -fno-builtin
  -fno-stack-protector -mno-red-zone -O2 -Wall -Wextra -Werror
  -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/libipc/include"
  -I "$protocol/include" -I "$protocol/generated/include")
sources=("$repo_root/userland/kobox2_adapter/ipc.c" "$repo_root/userland/kobox2_adapter/bootstrap.c"
  "$repo_root/userland/libpacha/src/syscall.c" "$protocol/src/closure_manifest.c"
  "$protocol/src/resource_grant.c" "$protocol/src/sha256.c")
"$compiler" "${common[@]}" -static -fno-pie -fno-pic \
  -I "$sandbox" "$repo_root/tests/native_kobox2_ipc_child.c" "${sources[@]}" \
  "$repo_root/userland/kobox2_adapter/package.c" "$sandbox/boot/package.c" \
  "$sandbox/arch/x86_64/elf.c" \
  -Wl,-T,"$repo_root/tests/native_kobox2_ipc_child.ld",-z,noexecstack \
  -o "$out/child.elf"
"$compiler" "${common[@]}" -static-pie -fPIE \
  -I "$repo_root/kobox2/include" \
  -DSEED0_BOOTFS_NO_DIAGNOSTICS \
  "$repo_root/tests/native_kobox2_ipc.c" \
  "$repo_root/userland/seed0boot/src/bootfs_reader.c" \
  "$repo_root/userland/gpud/process_native.c" \
  "$repo_root/userland/gpud/launch_native.c" \
  "$repo_root/userland/gpud/launch.c" "$repo_root/userland/gpud/process.c" \
  "$repo_root/kobox2/src/controller/controller.c" "$repo_root/kobox2/src/closure.c" \
  "$out/musl/obj/src/stdlib/qsort.o" "$out/musl/obj/src/stdlib/qsort_nr.o" \
  "$repo_root/userland/gpud/arch/x86_64/launch_image.c" \
  "$repo_root/tests/native_test_init.c" "${sources[@]}" \
  -Wl,-e,_start,-z,noexecstack -o "$out/native_kobox2_ipc.elf"
