#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

pacgo="$repo_root/.artifacts/bin/pacgo"
build_dir="$repo_root/.artifacts/cmake/seed0root"
out="$repo_root/.artifacts/test-results/gpud-restart"
mkdir -p "$out"

previous=OFF
if [[ -f "$build_dir/CMakeCache.txt" ]]; then
  value=$(sed -n 's/^SEED0ROOT_GPUD_RESTART_TEST:BOOL=//p' "$build_dir/CMakeCache.txt")
  [[ "$value" != ON ]] || previous=ON
fi

restore() {
  cmake -S userland/seed0root -B "$build_dir" \
    -DSEED0ROOT_GPUD_RESTART_TEST="$previous"
  cmake -E remove "$build_dir/seed0root.elf"
  "$pacgo" build userland seed0root --no-rootfs
  "$pacgo" sync bootfs --no-build
}
trap 'restore >"$out/restore.log" 2>&1' EXIT

cmake -S userland/seed0root -B "$build_dir" \
  -DSEED0ROOT_GPUD_RESTART_TEST=ON >"$out/configure.log" 2>&1
cmake -E remove "$build_dir/seed0root.elf"
"$pacgo" build userland seed0root --no-rootfs >"$out/build-seed0root.log" 2>&1
"$pacgo" build userland gpud --no-rootfs >"$out/build-gpud.log" 2>&1
"$pacgo" build userland lpr_drm_card0_smoke --no-rootfs \
  >"$out/build-client.log" 2>&1
"$pacgo" sync rootfs --force >"$out/rootfs.log" 2>&1
"$pacgo" sync bootfs --no-build >"$out/bootfs.log" 2>&1

"$pacgo" qemu-test \
  --console-shell \
  --timeout 180s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
  --send ': > /tmp/gpud-restart-ready; busybox timeout -s KILL 90 /cmd/lpr_drm_card0_smoke.elf --render --restart' \
  --expect '[gpud] ready generation=1' \
  --expect '[gpud] sandbox-force-killed generation=1' \
  --expect '[gpud] generation=1 retired next=2' \
  --expect '[gpud] ready generation=2' \
  --expect 'GPUD_SANDBOX_RESTART_TEST=PASS old=1 new=2' \
  --expect 'GPUD_RESTART_CLIENT_OK old_generation_retired=1 reopen=1' \
  --expect 'DRM_RENDER_OK name=virtio_gpu' \
  --expect 'generation=2 backend-close=1' >"$out/run.log" 2>&1

cp .artifacts/serial-tty-test.log "$out/serial.log"
cp .artifacts/console-tty-test.log "$out/console.log"
if rg -q '\[gpud\] stopped|GPUD_SANDBOX_RESTART_TEST=FAIL|DRM_RENDER_FAIL|PANIC' \
    "$out/serial.log" "$out/console.log"; then
  echo 'gpud restart regression: unexpected terminal error' >&2
  exit 1
fi

echo 'GPUD_RESTART_GATE=PASS generation=1->2 reopen=1'
