#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"
build_dir="$repo_root/.artifacts/cmake/seed0root"
out_dir="$repo_root/.artifacts/tests/native-host-primitives"
mkdir -p "$out_dir"
previous=OFF
if [[ -f "$build_dir/CMakeCache.txt" ]]; then
  value=$(sed -n 's/^SEED0ROOT_NATIVE_PRIMITIVES_TEST:BOOL=//p' "$build_dir/CMakeCache.txt")
  [[ "$value" != ON ]] || previous=ON
fi
restore() {
  cmake -S userland/seed0root -B "$build_dir" -DSEED0ROOT_NATIVE_PRIMITIVES_TEST="$previous" &&
    .artifacts/bin/pacgo build userland seed0root --no-rootfs &&
    .artifacts/bin/pacgo sync rootfs --no-build &&
    .artifacts/bin/pacgo sync bootfs --no-build
}
finish() {
  local result=$?
  trap - EXIT
  if ! restore >"$out_dir/restore.log" 2>&1; then result=1; fi
  exit "$result"
}
trap finish EXIT
.artifacts/bin/pacgo build kernel
for app in pachaos_musl_libc pachaos_musl_ldso linux_lpr_runtime \
  pachaos_capsule seed0boot storage_boot filed lpr_supervisor unixd termd netd drmd inputd; do
  if ! .artifacts/bin/pacgo build userland "$app" --no-rootfs >"$out_dir/build-$app.log" 2>&1; then
    tail -n 60 "$out_dir/build-$app.log"
    exit 1
  fi
done
cmake -S userland/seed0root -B "$build_dir" -DSEED0ROOT_NATIVE_PRIMITIVES_TEST=ON
.artifacts/bin/pacgo build userland native_host_primitives --no-rootfs
.artifacts/bin/pacgo build userland native_device_contract --no-rootfs
.artifacts/bin/pacgo build userland seed0root --no-rootfs
.artifacts/bin/pacgo sync rootfs --no-build
.artifacts/bin/pacgo sync bootfs --no-build
qemu_result=0
.artifacts/bin/pacgo qemu-test --console-shell --cpus 2 --timeout 45s \
  --qemu-arg=-device --qemu-arg='virtio-rng-pci,disable-legacy=on,iommu_platform=on' \
  --send ':' \
  --expect 'NATIVE_THREAD_NOTIFICATION=PASS' \
  --expect 'NATIVE_FAULT_RETURN=PASS' \
  --expect 'NATIVE_EXECUTION=PASS' \
  --expect 'NATIVE_CLOCK_CONTRACT=PASS' \
  --expect 'NATIVE_DEVICE_CONTRACT=PASS' || qemu_result=$?
cp .artifacts/serial-tty-test.log "$out_dir/serial.log"
[[ "$qemu_result" == 0 ]] || exit "$qemu_result"
if rg -n 'INVALID OPCODE|USER fault|NATIVE_[A-Z_]+=FAIL|Kernel panic|PANIC' "$out_dir/serial.log"; then
  echo "Native primitive test contained an unexpected fault or failure" >&2
  exit 1
fi
