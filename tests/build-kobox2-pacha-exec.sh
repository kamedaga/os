#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/kobox2-pacha-exec"
mkdir -p "$out"
client="$repo_root/kobox2/linux-sandbox/kobox/host/posix/elf_client_test"
extra=()
case "${KOBOX_PACHA_EXEC_CASE:-base}" in
  base) ;;
  syscall-range)
    extra+=("$repo_root/tests/kobox2_exec_syscall_range.c"
      -Wl,--wrap=kobox_elf_client_test)
    ;;
  signals)
    extra+=(-DKOBOX_ELF_SIGNALS=1
      "$(dirname "$client")/signal_client_test.c" "$(dirname "$client")/signal_client_test.S")
    ;;
  vfork)
    extra+=(-DKOBOX_ELF_VFORK=1
      "$(dirname "$client")/vfork_client_test.c" "$(dirname "$client")/vfork_client_test.S")
    ;;
  *) echo "Unknown exec case" >&2; exit 2 ;;
esac
"${CAPOS_FREESTANDING_CC:-clang}" -target x86_64-linux-gnu -fuse-ld=lld \
  -nostdlib -static -ffreestanding -fno-builtin -fno-stack-protector \
  -mno-red-zone -std=c11 -O2 -g -Wall -Wextra -Werror \
  "$client.c" "$client.S" "${extra[@]}" -Wl,-e,_start,-z,noexecstack \
  -o "$out/linux_client.elf"

# Build the existing LPR decoder from its pinned source, not from potentially
# stale objects produced by another runtime build.
zydis_source="$(bash "$repo_root/pack/scripts/download_zydis.sh")"
zydis_flags=(-DZYDIS_STATIC_BUILD -DZYDIS_MINIMAL_MODE -DZYDIS_DISABLE_ENCODER
  -DZYDIS_DISABLE_FORMATTER -DZYDIS_DISABLE_SEGMENT -DZYAN_NO_LIBC)
for source in Decoder DecoderData SharedData; do
  "${CAPOS_FREESTANDING_CC:-clang}" -target x86_64-linux-gnu \
    -std=c11 -O2 -ffreestanding -fPIC -fvisibility=hidden -mno-red-zone \
    -fno-stack-protector -fno-builtin "${zydis_flags[@]}" \
    -I "$zydis_source/include" -I "$zydis_source/src" \
    -I "$zydis_source/dependencies/zycore/include" \
    -c "$zydis_source/src/$source.c" -o "$out/$source.o"
done
"${CAPOS_FREESTANDING_CC:-clang}" -target x86_64-linux-gnu \
  -std=c11 -O2 -ffreestanding -fPIC -fvisibility=hidden -mno-red-zone \
  -fno-stack-protector -fno-builtin -Wall -Wextra -Werror "${zydis_flags[@]}" \
  -I "$zydis_source/include" -I "$zydis_source/src" \
  -I "$zydis_source/dependencies/zycore/include" \
  -I "$repo_root/userland/personality/include" \
  -I "$repo_root/userland/libipc/include" \
  -c "$repo_root/userland/personality/linux/decoder/patch_scan.c" -o "$out/patch_scan.o"
