#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Native capture bootstrap and the existing decoder, without fixture programs.
set -euo pipefail
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
adapter="$repo_root/userland/kobox2_adapter"
out="$repo_root/.artifacts/kobox2-userspace-support"
compiler="${CAPOS_FREESTANDING_CC:-clang}"
extra=()
if [[ "${KOBOX_USER_TRACE:-0}" == 1 ]]; then extra+=(-DPH_VM_CLIENT_TRACE=1); fi
mkdir -p "$out"
"$compiler" -E -P -x c -I "$repo_root/kobox2/linux-sandbox/kobox" \
    -I "$adapter" "$adapter/vm_client.ld" -o "$out/bootstrap.ld"
"$compiler" -target x86_64-linux-gnu -fuse-ld=lld -nostdlib -static \
    -ffreestanding -fno-builtin -fno-stack-protector -fPIE -mno-red-zone \
    -mgeneral-regs-only -std=c11 -O2 -g -Wall -Wextra -Werror "${extra[@]}" \
    -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/libipc/include" \
    -I "$repo_root/userland/personality/include" \
    -I "$repo_root/userland/personality/linux/runtime" \
    -I "$repo_root/kobox2/linux-sandbox/kobox" -I "$adapter" \
    "$adapter/vm_client.c" "$adapter/vm_client_entry.S" \
    "$repo_root/userland/libpacha/src/syscall.c" \
    -Wl,-T,"$out/bootstrap.ld",-z,noexecstack -o "$out/bootstrap.elf"
"$compiler" -target x86_64-linux-gnu -fuse-ld=lld -nostdlib -static \
    -ffreestanding -fno-builtin -fno-stack-protector -fPIE -mno-red-zone \
    -mgeneral-regs-only -std=c11 -O2 -g -Wall -Wextra -Werror \
    -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/libipc/include" \
    -I "$adapter" "$adapter/vm_guardian.c" "$adapter/vm_guardian_entry.S" \
    "$repo_root/userland/libpacha/src/syscall.c" \
    -Wl,-T,"$out/bootstrap.ld",-z,noexecstack -o "$out/guardian.elf"
cat > "$out/guardian_image.S" <<EOF
.section .rodata.ph_vm_guardian,"a",@progbits
.balign 16
.global ph_vm_guardian_image_start, ph_vm_guardian_image_end
ph_vm_guardian_image_start:
.incbin "$out/guardian.elf"
ph_vm_guardian_image_end:
.section .note.GNU-stack,"",@progbits
EOF
zydis_source=$(bash "$repo_root/pack/scripts/download_zydis.sh")
flags=(-target x86_64-linux-gnu -std=c11 -O2 -ffreestanding -fPIC \
    -fvisibility=hidden -mno-red-zone -fno-stack-protector -fno-builtin \
    -DZYDIS_STATIC_BUILD -DZYDIS_MINIMAL_MODE -DZYDIS_DISABLE_ENCODER \
    -DZYDIS_DISABLE_FORMATTER -DZYDIS_DISABLE_SEGMENT -DZYAN_NO_LIBC \
    -I "$zydis_source/include" -I "$zydis_source/src" \
    -I "$zydis_source/dependencies/zycore/include")
objects=()
for source in Decoder DecoderData SharedData; do
    "$compiler" "${flags[@]}" -c "$zydis_source/src/$source.c" -o "$out/$source.o"
    objects+=("$out/$source.o")
done
"$compiler" "${flags[@]}" -Wall -Wextra -Werror \
    -I "$repo_root/userland/personality/include" \
    -I "$repo_root/userland/libipc/include" \
    -c "$repo_root/userland/personality/linux/decoder/patch_scan.c" \
    -o "$out/patch_scan.o"
ar rcs "$out/decoder.a" "${objects[@]}" "$out/patch_scan.o"
if nm --defined-only "$out/bootstrap.elf" | rg 'ph_vm_fixture_|kobox_.*test'; then
    echo 'Fixture implementation entered the production bootstrap' >&2
    exit 1
fi
sha256sum "$out/bootstrap.elf" "$out/guardian.elf" "$out/decoder.a" > "$out/SHA256SUMS"
