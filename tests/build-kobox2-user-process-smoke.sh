#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
adapter="$repo_root/userland/kobox2_adapter"
out="${KOBOX_USER_OUT:-$repo_root/.artifacts/tests/kobox2-user-process-smoke}"
compiler="${CAPOS_FREESTANDING_CC:-clang}"
mkdir -p "$out"
python3 "$repo_root/tests/build-kobox2-crypto-smoke.py"
python3 - "$repo_root" "$out" <<'PY'
import hashlib,json,sys
from pathlib import Path
root,out=map(Path,sys.argv[1:])
inventory=json.loads((root/'.artifacts/kobox2-wifi-profile/closure-inventory.json').read_text())
names=[Path(name).stem for name in inventory['load_order']]
assert len(names)==len(set(names))==8
import os
if os.environ.get('KOBOX_USER_RADIO_MODULE'):
 names.append('mac80211_hwsim')
firmware=(root/'.artifacts/kobox2-wifi-production/firmware.bundle').read_bytes()
(out/'wifi_modules.h').write_text('static const char *const module_names[] = {'+
    ','.join(json.dumps(name) for name in names)+'};\n'+
    'static const unsigned char firmware_digest[32] = {'+
    ','.join(str(b) for b in hashlib.sha256(firmware).digest())+'};\n')
PY
bash "$repo_root/pack/scripts/build_kobox_userspace_support.sh"
sources=(runtime native wait_retained machine image exception boot bootfs entropy firmware_bundle
    vm vm_process vm_syscall vm_context vm_execution vm_lpr vm_memory vm_clone
    exec_image exec_fault)
inputs=()
for name in "${sources[@]}"; do inputs+=("$adapter/$name.c"); done
"$compiler" -target x86_64-linux-gnu -fuse-ld=lld -nostdlib -static-pie \
    -ffreestanding -fno-builtin -fno-stack-protector -fPIE -mno-red-zone \
    -DPH_VM_USER_PROCESS=1 -DPH_USER_PROCESS_SMOKE=1 \
    -DPH_USER_WIFI_RADIO=$([[ -n "${KOBOX_USER_RADIO_MODULE:-}" ]] && echo 1 || echo 0) \
    -std=c11 -O2 -g -Wall -Wextra -Werror \
    -I "$adapter" -I "$repo_root/kobox2/linux-sandbox/kobox" -I "$out" \
    -I "$repo_root/kobox2/protocol/include" \
    -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/libipc/include" \
    -I "$repo_root/userland/libcapsule/include" -I "$repo_root/userland/personality/include" \
    -I "$repo_root/userland/personality/linux/decoder" \
    "$repo_root/tests/kobox2_user_process_smoke.c" "${inputs[@]}" \
    "$adapter/entry.S" "$adapter/fiber.S" \
    "$repo_root/.artifacts/kobox2-userspace-support/guardian_image.S" \
    "$repo_root/userland/libpacha/src/syscall.c" \
    "$repo_root/userland/personality/linux/runtime/lpr_zpoline.c" \
    "$repo_root/kobox2/linux-sandbox/kobox/boot/core.c" \
    "$repo_root/kobox2/linux-sandbox/kobox/boot/fixed_image.c" \
    "$repo_root/kobox2/linux-sandbox/kobox/arch/x86_64/elf.c" \
    "$repo_root/kobox2/linux-sandbox/kobox/machine/domain.c" \
    "$repo_root/kobox2/protocol/src/sha256.c" \
    "$repo_root/.artifacts/kobox2-userspace-support/decoder.a" \
    -Wl,-e,_start,-z,noexecstack -o "$out/init.elf"
