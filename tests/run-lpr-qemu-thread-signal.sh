#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/lpr-thread-signal-gate
disk='.artifacts/disk.img?offset=202375168'
mkdir -p "$out"
if fuser .artifacts/disk.img; then
  echo 'disk.img is in use' >&2
  exit 1
fi
bash userland/personality/linux/build-lpr.sh .artifacts/lpr-linux-x86_64.so >"$out/lpr-build.log" 2>&1
bash pack/scripts/build_lpr_pthread_smoke.sh .artifacts/userland-fixtures/lpr_thread_signal \
  userland/fixtures/src/wsl_musl/lpr_thread_signal.c >"$out/fixture-build.log" 2>&1
bash pack/scripts/build_lpr_pthread_smoke.sh .artifacts/userland-fixtures/lpr_service_account \
  userland/fixtures/src/wsl_musl/lpr_service_account.c >>"$out/fixture-build.log" 2>&1
cmake -S userland/seed0root -B .artifacts/cmake/seed0root-thread-signal \
  -DSEED0ROOT_LPR_THREAD_SIGNAL_TEST=ON >"$out/seed-build.log" 2>&1
cmake --build .artifacts/cmake/seed0root-thread-signal --parallel 2 >>"$out/seed-build.log" 2>&1

# Update only the test inputs; do not copy rootfs or restore patched DRM.
replace_guest() {
  debugfs -w -R "rm $2" "$disk"
  debugfs -w -R "write $1 $2" "$disk"
  debugfs -w -R "set_inode_field $2 size $(stat -c %s "$1")" "$disk"
  debugfs -w -R "set_inode_field $2 mode 0100755" "$disk"
}
debugfs -R "dump /sbin/seed0root.elf $out/seed-original.elf" "$disk"
test -s "$out/seed-original.elf"
trap 'replace_guest "$out/seed-original.elf" /sbin/seed0root.elf >"$out/restore.log" 2>&1' EXIT
replace_guest .artifacts/cmake/seed0root-thread-signal/seed0root.elf /sbin/seed0root.elf
replace_guest .artifacts/lpr-linux-x86_64.so /lib/pacha/lpr-linux-x86_64.so
for path in /lib/linux/libc.so /lib/linux/ld-musl-x86_64.so.1 /lib/libc.musl-x86_64.so.1; do
  replace_guest .artifacts/userland-fixtures/lpr-linux-musl-libc.so "$path"
done
replace_guest .artifacts/userland-fixtures/lpr_thread_signal.dynamic.elf /cmd/lpr_thread_signal.elf
replace_guest .artifacts/userland-fixtures/lpr_service_account.dynamic.elf /cmd/lpr_service_account.elf

status=0
MESA_D3D12_DEFAULT_ADAPTER_NAME=Intel GALLIUM_DRIVER=d3d12 GDK_BACKEND=x11 \
  .artifacts/bin/pacgo qemu-test --cpus 4 --graphics virgl --display gtk \
  --qemu-arg=-vga --qemu-arg=none --qemu-arg=-device --qemu-arg=ramfb \
  --timeout 45s --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
  --expect 'LPR_THREAD_SIGNAL_BUSY_AND_MAIN=OK' \
  --expect 'LPR_THREAD_SIGNAL_MASKED=OK' \
  --expect 'LPR_THREAD_SIGNAL_MUSL_SYNCCALL=OK' \
  --expect 'LPR_THREAD_SIGNAL_FORK_EXIT=OK' \
  --expect 'LPR_THREAD_SIGNAL_GROUP_KILL=OK' \
  --expect 'LPR_THREAD_SIGNAL=OK' \
  --expect 'LPR_SERVICE_TRANSITIONS=OK' \
  --expect 'LPR_PTHREAD_POST_DETACHED_CREATE_JOIN=OK' \
  --expect 'SIGNAL_OWNER_DONE' >"$out/qemu.log" 2>&1 || status=$?
cp .artifacts/serial-tty-test.log "$out/serial.log"
cp .artifacts/console-tty-test.log "$out/console.log"
exit "$status"
