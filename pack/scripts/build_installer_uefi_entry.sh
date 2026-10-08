#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
output="${1:-$repo_root/.artifacts/installer-uefi/BOOTX64.EFI}"
object="${output%.EFI}.obj"
mkdir -p "$(dirname "$output")"

clang --target=x86_64-pc-windows-msvc -std=c11 -ffreestanding \
    -fno-builtin -fno-stack-protector -fshort-wchar -Os -c \
    "$repo_root/bootloader/uefi/installer_entry.c" -o "$object"
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib \
    /machine:x64 /out:"$output" "$object"

file "$output" | grep -q 'PE32+ executable.*EFI application'
test -s "$output"
printf '%s\n' "$output"
