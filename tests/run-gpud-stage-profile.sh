#!/usr/bin/env bash
# Diagnostic binaries are generated separately. Always restore the installed pair.
set -euo pipefail
cd "$(dirname "$0")/.."
repo=$PWD
out="$repo/.artifacts/gpu-stage-profile/${1:?case required}"
mkdir "$out"
cp .artifacts/userland/gpud/GPUD.ELF "$out/restore-gpud.elf"
cp .artifacts/userland/gpud_sandbox/GPUDSBX.ELF "$out/restore-sandbox.elf"
restore() {
  cp "$out/restore-gpud.elf" .artifacts/userland/gpud/GPUD.ELF
  cp "$out/restore-sandbox.elf" .artifacts/userland/gpud_sandbox/GPUDSBX.ELF
  .artifacts/bin/pacgo sync rootfs --no-build --force
}
trap 'restore >"$out/restore.log" 2>&1' EXIT
cp .artifacts/gpu-stage-profile/build/gpud.elf .artifacts/userland/gpud/GPUD.ELF
cp .artifacts/gpu-stage-profile/build/sandbox.elf .artifacts/userland/gpud_sandbox/GPUDSBX.ELF
.artifacts/bin/pacgo sync rootfs --no-build --force >"$out/sync.log" 2>&1
set +e
MESA_D3D12_DEFAULT_ADAPTER_NAME=Intel GDK_BACKEND=x11 GALLIUM_DRIVER=d3d12 \
  FISHBOWL_BUTTON=1 INTERACTION_OUT="$out" \
  setsid .artifacts/bin/pacgo qemu-test --cpus 4 --timeout 200s \
  --graphics virgl --display gtk --input-profile keyboard-tablet \
  --qemu-arg=-vga --qemu-arg=none --qemu-arg=-device --qemu-arg=ramfb \
  --qemu-arg=-qmp --qemu-arg="unix:$out/qmp.sock,server=on,wait=off" \
  --python "$repo/tests/qemu_xfce_interaction.py" >"$out/run.log" 2>&1
result=$?
set -e
cp .artifacts/serial-tty-test.log "$out/serial.log"
cp .artifacts/console-tty-test.log "$out/console.log"
cp .artifacts/qemu-tty-python.log "$out/observer.log"
debugfs -R 'cat /var/log/Xorg.0.log' '.artifacts/disk.img?offset=202375168' \
  >"$out/Xorg.0.log" 2>"$out/debugfs.log"
grep -F 'glamor X acceleration enabled on virgl (D3D12 (Intel(R) Graphics))' "$out/Xorg.0.log" || result=1
exit "$result"
