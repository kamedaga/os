#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
out="$root/.artifacts/tests/filed-optional-module-image-unit"
mkdir -p "$out"
clang -std=c11 -O1 -g -Wall -Wextra -Werror \
  -I"$root/userland/libfiled_client/include" -I"$root/userland/filed/include" \
  -I"$root/userland/libpacha/include" -I"$root/userland/libipc/include" \
  -I"$root/kobox2/protocol/include" -I"$root/kobox2/protocol/generated/include" \
  "$root/tests/filed_optional_module_image_unit.c" -o "$out/test"
sha256sum "$root/userland/libfiled_client/src/module_image.c" \
  "$root/tests/filed_optional_module_image_unit.c" > "$out/inputs.sha256"
timeout 10 "$out/test" 2>&1 | tee "$out/result.log"
