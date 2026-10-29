#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$repo_root"
mkdir -p .artifacts/filed-tests
zydis="$(bash pack/scripts/download_zydis.sh)"
/usr/bin/clang -std=c11 -O2 -Wall -Wextra -Werror \
  -DZYDIS_STATIC_BUILD -DZYDIS_MINIMAL_MODE -DZYDIS_DISABLE_ENCODER \
  -DZYDIS_DISABLE_FORMATTER -DZYDIS_DISABLE_SEGMENT -DZYAN_NO_LIBC \
  -Iuserland/filed/include -Iuserland/personality/include \
  -Iuserland/personality/linux/decoder -Iuserland/libpacha/include \
  -Iuserland/libipc/include \
  -Ikobox2/protocol/include -Ikobox2/protocol/generated/include \
  -I"$zydis/include" -I"$zydis/src" -I"$zydis/dependencies/zycore/include" \
  userland/filed/tests/manifest_dso_test.c userland/filed/src/manifest.c \
  userland/personality/linux/decoder/patch_scan.c \
  kobox2/protocol/src/sha256.c \
  "$zydis/src/Decoder.c" "$zydis/src/DecoderData.c" \
  "$zydis/src/SharedData.c" -o .artifacts/filed-tests/manifest-dso
.artifacts/filed-tests/manifest-dso "$@"
