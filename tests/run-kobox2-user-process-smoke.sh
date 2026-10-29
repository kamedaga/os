#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out="${KOBOX_USER_OUT:-$repo_root/.artifacts/tests/kobox2-user-process-smoke}"
core="${KOBOX_USER_CORE:-$repo_root/.artifacts/kobox2-wifi-production/runtime/linux-boot-runtime.so}"
kernel="${KOBOX_USER_KERNEL_ELF:-$repo_root/kernel/zig-out/bin/limine/pacha-kernel.elf}"
cpus="${KOBOX_USER_CPUS:-2}"
[[ "$cpus" =~ ^[1-9][0-9]*$ && "$cpus" -le 8 ]]
test -f "$core" && test -f "$kernel"
bash "$repo_root/tests/build-kobox2-user-process-smoke.sh"
cat > "$out/bootfs.manifest" <<EOF
/srv/kobox2/core.so=$core
/srv/kobox2/bootstrap.elf=$repo_root/.artifacts/kobox2-userspace-support/bootstrap.elf
/srv/kobox2/wpa_supplicant=$repo_root/.artifacts/kobox2-wifi-production/userspace/wpa_supplicant
/srv/kobox2/firmware.bundle=$repo_root/.artifacts/kobox2-wifi-production/firmware.bundle
/srv/kobox2/crypto-smoke.elf=$repo_root/.artifacts/tests/kobox2-crypto-smoke/crypto-smoke.elf
EOF
python3 - "$repo_root" "$out/bootfs.manifest" <<'PY'
import json,sys
from pathlib import Path
root,manifest=map(Path,sys.argv[1:])
inventory=json.loads((root/'.artifacts/kobox2-wifi-profile/closure-inventory.json').read_text())
with manifest.open('a') as stream:
 for name in inventory['load_order']:
  base=Path(name).name
  stream.write(f'/srv/kobox2/modules/{base}={root}/.artifacts/kobox2-wifi-production/linux-package/modules/{base}\n')
PY
if [[ -n "${KOBOX_USER_RADIO_MODULE:-}" ]]; then
    test -f "$KOBOX_USER_RADIO_MODULE"
    printf '/srv/kobox2/modules/mac80211_hwsim.ko=%s\n' "$KOBOX_USER_RADIO_MODULE" >> "$out/bootfs.manifest"
fi
(cd "$repo_root/pack" && go run ./cmd/bootfs-image \
    -manifest "$out/bootfs.manifest" -output "$out/bootfs.img")
# This 64 MiB image contains boot files only; it has no rootfs or user disk.
cp --reflink=auto "$repo_root/.artifacts/limine-boot.img" "$out/boot.img"
mcopy -o -i "$out/boot.img@@2097152" "$kernel" ::/KERNEL.ELF
mcopy -o -i "$out/boot.img@@2097152" "$out/init.elf" ::/INITAPP.ELF
mcopy -o -i "$out/boot.img@@2097152" "$out/bootfs.img" ::/BOOTFS.IMG
sha256sum "$core" "$kernel" "$out/init.elf" "$out/bootfs.img" > "$out/inputs.sha256"
: > "$out/serial.log"
qemu-system-x86_64 -machine q35 -cpu host -enable-kvm -m 2G -smp "$cpus" \
    -device intel-iommu,intremap=off,aw-bits=48 \
    -drive "file=$out/boot.img,format=raw,if=ide" \
    -display none -serial "file:$out/serial.log" -monitor none -no-reboot \
    > "$out/qemu.log" 2>&1 &
qemu_pid=$!
stop() { kill "$qemu_pid" 2>/dev/null || true; wait "$qemu_pid" || true; }
trap stop EXIT
deadline=$((SECONDS + ${KOBOX_USER_TIMEOUT:-120}))
while kill -0 "$qemu_pid" 2>/dev/null; do
    if rg -q 'USER_PROCESS_SMOKE=PASS|PACHA_KOBOX_FOUNDATION=FAIL|PANIC|ACTION=terminate' "$out/serial.log"; then break; fi
    [[ "$SECONDS" -lt "$deadline" ]] || break
    sleep .2
done
stop
trap - EXIT
for marker in HOST_ENTROPY ADMISSION REAL_WPA_ELF WPA_CONTROL CRYPTO EXEC_ROLLBACK LIFELINE MODULE_DRAIN SMOKE; do
    rg -q "USER_PROCESS_$marker=PASS" "$out/serial.log"
done
if rg -n 'PACHA_KOBOX_FOUNDATION=FAIL|PANIC|ACTION=terminate' "$out/serial.log"; then exit 1; fi
echo "Production userspace runner smoke passed: $out/serial.log"
