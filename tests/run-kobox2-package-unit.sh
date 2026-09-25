#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
ulimit -c 0
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/kobox2-package-unit/${CC:-cc}"
mkdir -p "$out"
sandbox="$repo_root/kobox2/linux-sandbox/kobox"
protocol="$repo_root/kobox2/protocol"
sources=("$repo_root/tests/kobox2_package_unit.c" "$repo_root/userland/kobox2_adapter/package.c"
  "$sandbox/boot/package.c" "$sandbox/arch/x86_64/elf.c"
  "$protocol/src/closure_manifest.c" "$protocol/src/resource_grant.c" "$protocol/src/sha256.c")
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -Wpedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/libipc/include" \
  -I "$sandbox" -I "$protocol/include" -I "$protocol/generated/include" \
  "${sources[@]}" -o "$out/package-unit"
"$out/package-unit" | tee "$out/result.log"
sha256sum "${sources[@]}" "$repo_root/userland/kobox2_adapter/package.h" \
  "$repo_root/userland/kobox2_adapter/bootstrap.h" "$repo_root/tests/run-kobox2-package-unit.sh" \
  >"$out/inputs.sha256"
