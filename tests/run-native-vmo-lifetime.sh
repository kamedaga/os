#!/usr/bin/env bash
# Boot the probe as the native init application. Running it through a Linux
# shell routes its native syscall numbers through LPR and is not a valid test.
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/native-vmo-lifetime"
mkdir -p "$out"
"${CAPOS_FREESTANDING_CC:-clang}" -target x86_64-linux-gnu -fuse-ld=lld \
    -nostdlib -static-pie -fPIE -ffreestanding -fno-builtin -ffunction-sections \
    -fno-stack-protector -mno-red-zone -O2 -Wall -Wextra -Werror \
    -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/libipc/include" \
    "$repo_root/tests/native_vmo_lifetime.c" "$repo_root/userland/libpacha/src/syscall.c" \
    -Wl,-e,_start,-z,noexecstack,--gc-sections -o "$out/probe.elf"
cp "$repo_root/.artifacts/limine-boot.img" "$out/boot.img"
mcopy -o -i "$out/boot.img@@2097152" \
    "${CAPOS_NATIVE_VMO_KERNEL:-$repo_root/kernel/zig-out/bin/limine/pacha-kernel.elf}" ::/KERNEL.ELF
mcopy -o -i "$out/boot.img@@2097152" "$out/probe.elf" ::/INITAPP.ELF
python3 - "$out" <<'PY'
from pathlib import Path
import subprocess
import sys
import time

out = Path(sys.argv[1])
log = out/'serial.log'
with log.open('wb') as stream:
    proc = subprocess.Popen(['qemu-system-x86_64','-machine','q35','-enable-kvm',
        '-cpu','host','-m','2G','-smp','4','-display','none','-monitor','none',
        '-device','intel-iommu,intremap=off,aw-bits=48',
        '-drive',f'file={out}/boot.img,format=raw,if=ide,snapshot=on',
        '-serial','stdio','-no-reboot','-net','none'], stdout=stream, stderr=stream)
    try:
        deadline = time.monotonic()+60
        while time.monotonic()<deadline:
            text = log.read_text(errors='replace')
            if 'NATIVE_VMO_LIFETIME=PASS' in text or 'NATIVE_VMO_LIFETIME=FAIL' in text:
                break
            if proc.poll() is not None:
                break
            time.sleep(.1)
    finally:
        if proc.poll() is None:
            proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
text = log.read_text(errors='replace')
for line in text.splitlines():
    if 'NATIVE_VMO_' in line:
        print(line)
assert 'NATIVE_VMO_LIFETIME=PASS' in text and 'NATIVE_VMO_CHUNKS=PASS' in text and 'NATIVE_VMO_DIRTY=PASS' in text, text[-6000:]
assert 'NATIVE_VMO_LIFETIME=FAIL' not in text
PY
