#!/usr/bin/env bash
# Build a fresh boot-only image; QEMU writes solely to a disposable overlay.
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/native-pager-fault"
mkdir -p "$out"
bash "$repo_root/tests/build-native-pager-fault.sh"
(cd "$repo_root/kernel" && zig build limine)
printf '/fixture=../../../kernel/CONTRIBUTING.md\n' > "$out/minimal.bootfs"
(cd "$repo_root/pack" && go run ./cmd/bootfs-image --manifest "$out/minimal.bootfs" --output "$out/bootfs.img")
(cd "$repo_root/pack" && go run ./cmd/pager-image "$repo_root" "$out")
qemu-img create -f qcow2 -F raw -b "$out/boot.img" "$out/boot-overlay.qcow2" > "$out/overlay.log"
: > "$out/serial.log"
rm -f "$out/hmp.sock"
qemu-system-x86_64 -machine q35 -cpu host -enable-kvm -m 2G -smp 2 \
  -device intel-iommu,intremap=off,aw-bits=48 \
  -drive "file=$out/boot-overlay.qcow2,format=qcow2,if=ide" -display none \
  -serial "file:$out/serial.log" -monitor "unix:$out/hmp.sock,server=on,wait=off" \
  -no-reboot -net none >"$out/qemu.log" 2>&1 &
qemu_pid=$!
stop_qemu() { if kill -0 "$qemu_pid" 2>/dev/null; then kill "$qemu_pid"; fi; wait "$qemu_pid" || true; }
trap stop_qemu EXIT
deadline=$((SECONDS + 90))
while kill -0 "$qemu_pid" 2>/dev/null && (( SECONDS < deadline )); do
  if rg -q 'NATIVE_PAGER=(PASS|FAIL)|PANIC|ACTION=terminate' "$out/serial.log"; then break; fi
  sleep 0.2
done
if kill -0 "$qemu_pid" 2>/dev/null; then
  python3 - "$out/hmp.sock" "$out/screen.ppm" <<'PY'
import socket
import sys
from pathlib import Path
import time
with socket.socket(socket.AF_UNIX) as monitor:
    monitor.settimeout(2)
    monitor.connect(sys.argv[1])
    monitor.recv(4096)
    monitor.sendall(f'screendump {sys.argv[2]}\n'.encode())
    response = bytearray()
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if Path(sys.argv[2]).exists():
            break
        try:
            response.extend(monitor.recv(4096))
        except TimeoutError:
            break
    if not Path(sys.argv[2]).exists():
        raise RuntimeError(f'screendump failed: {response.decode(errors="replace")[-400:]}')
PY
fi
stop_qemu
trap - EXIT
if ! rg -q 'NATIVE_PAGER=PASS' "$out/serial.log"; then tail -100 "$out/serial.log"; exit 1; fi
if rg -n 'NATIVE_PAGER=FAIL|PANIC|ACTION=terminate' "$out/serial.log"; then exit 1; fi
printf 'Native pager fault passed: %s\n' "$out/serial.log"
