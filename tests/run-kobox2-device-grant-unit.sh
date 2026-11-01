#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
ulimit -c 0
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/kobox2-device-grant-unit/${CC:-cc}"
mkdir -p "$out"
sources=("$repo_root/tests/kobox2_device_grant_unit.c"
  "$repo_root/userland/kobox2_adapter/device_grant.c"
  "$repo_root/kobox2/protocol/src/closure_manifest.c"
  "$repo_root/kobox2/protocol/src/resource_grant.c" "$repo_root/kobox2/protocol/src/sha256.c")
for profile in default i915; do
flags=()
if [[ "$profile" == i915 ]]; then flags=(-DPH_SANDBOX_I915=1); fi
"${CC:-cc}" "${flags[@]}" -std=c11 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$repo_root/kobox2/linux-sandbox/kobox" -I "$repo_root/kobox2/protocol/include" \
  -I "$repo_root/kobox2/protocol/generated/include" \
  -I "$repo_root/userland/libpacha/include" -I "$repo_root/userland/libipc/include" \
  -I "$repo_root/userland/libcapsule/include" \
  "${sources[@]}" -o "$out/device-grant-unit-$profile"
"$out/device-grant-unit-$profile" | tee "$out/result-$profile.log"
done
sha256sum "${sources[@]}" "$repo_root/userland/kobox2_adapter/device_grant.h" \
  "$repo_root/userland/kobox2_adapter/device_authority.h" \
  "$repo_root/tests/run-kobox2-device-grant-unit.sh" >"$out/inputs.sha256"
