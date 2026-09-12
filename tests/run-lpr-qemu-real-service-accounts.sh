#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"
pacgo="$repo_root/.artifacts/bin/pacgo"
build_dir="$repo_root/.artifacts/cmake/seed0root"
out="$repo_root/.artifacts/test-results/real-service-automated"
mkdir -p "$out"
previous=OFF
if [[ -f "$build_dir/CMakeCache.txt" ]]; then
    value=$(sed -n 's/^SEED0ROOT_REAL_SERVICE_TEST:BOOL=//p' "$build_dir/CMakeCache.txt")
    [[ "$value" != ON ]] || previous=ON
fi
restore() {
    cmake -S userland/seed0root -B "$build_dir" -DSEED0ROOT_REAL_SERVICE_TEST="$previous" &&
        "$pacgo" build userland seed0root --no-rootfs &&
        "$pacgo" sync bootfs --no-build
}
trap 'restore >"$out/restore.log" 2>&1' EXIT
"$pacgo" build kernel >"$out/build-kernel.log" 2>&1
cmake -S userland/seed0root -B "$build_dir" -DSEED0ROOT_REAL_SERVICE_TEST=ON >"$out/configure.log" 2>&1
for app in seed0root linux_lpr_runtime service_runtime passwd group dbus_system_config dbus_account_test_config lpr_real_service lpr_madvise lpr_random system_services_test; do
    "$pacgo" build userland "$app" --no-rootfs >"$out/build-$app.log" 2>&1
done
"$pacgo" gen manifests >"$out/manifests.log" 2>&1
"$pacgo" sync bootfs --no-build >"$out/bootfs.log" 2>&1
"$pacgo" qemu-test --console-shell --cpus 4 --timeout 90s \
    --send '/cmd/lpr_random.elf && /cmd/lpr_madvise.elf && /bin/sh /cmd/system_services.sh && /cmd/lpr_accounts.elf' \
    --expect LPR_RANDOM=OK \
    --expect LPR_MADVISE=OK \
    --expect SYSTEM_SERVICES=OK \
    --expect SYSTEM_BUS=OK \
    --expect REAL_SERVICE_ALLOWED=OK \
    --expect REAL_SERVICE_DENIED_USER=OK \
    --expect REAL_SERVICE_DENIED_ROOT=OK \
    --expect REAL_SERVICE_DAEMON_FORK=OK \
    --expect LPR_ACCOUNTS=OK >"$out/run.log" 2>&1
cp .artifacts/serial-tty-test.log "$out/serial.log"
cp .artifacts/console-tty-test.log "$out/console.log"
if rg -i 'CRITICAL|Failed to start message bus|permission of the setuid helper|failed to get polkit authority|Unknown username|file_image_pagein' "$out/console.log"; then
    echo 'real-service regression: unexpected runtime error' >&2
    exit 1
fi
