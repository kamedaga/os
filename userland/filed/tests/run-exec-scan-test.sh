#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$repo_root"
mkdir -p .artifacts/filed-tests
zydis="$(bash pack/scripts/download_zydis.sh)"
/usr/bin/clang -std=c11 -D_GNU_SOURCE -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections -Wall -Wextra -Werror \
  -DZYDIS_STATIC_BUILD -DZYDIS_MINIMAL_MODE -DZYDIS_DISABLE_ENCODER \
  -DZYDIS_DISABLE_FORMATTER -DZYDIS_DISABLE_SEGMENT -DZYAN_NO_LIBC \
  -Iuserland/filed/include -Iuserland/personality/include \
  -Iuserland/personality/linux/decoder -Iuserland/libpacha/include \
  -Iuserland/libipc/include -Iuserland/storage/include \
  -Ikobox2/protocol/include -Ikobox2/protocol/generated/include \
  -I"$zydis/include" -I"$zydis/src" -I"$zydis/dependencies/zycore/include" \
  userland/filed/tests/exec_scan_test.c userland/filed/src/exec/linux_lpr/elf.c \
  userland/personality/linux/decoder/patch_scan.c \
  kobox2/protocol/src/sha256.c \
  "$zydis/src/Decoder.c" "$zydis/src/DecoderData.c" \
  "$zydis/src/SharedData.c" -o .artifacts/filed-tests/exec-scan
.artifacts/filed-tests/exec-scan "$@"
