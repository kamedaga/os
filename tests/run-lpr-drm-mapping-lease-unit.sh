#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/lpr-drm-mapping-lease"
mkdir -p "$out"
includes=()
for module in libipc libpacha personality daemons/common filed termd gpud inputd netd unixd lpr_supervisor; do
  includes+=("-I$repo_root/userland/$module/include")
done
/usr/bin/clang -std=c11 -O1 -g -Wall -Wextra -Werror \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -fsanitize=address,undefined -pthread "${includes[@]}" \
  -I"$repo_root/musl/pachaos/include" \
  "$repo_root/tests/lpr_drm_mapping_lease_unit.c" -o "$out/unit"
"$out/unit"
/usr/bin/clang -std=c11 -O1 -g -Wall -Wextra -Werror \
  -DLPR_UNMAP_PROFILE=1 \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -fsanitize=address,undefined -pthread "${includes[@]}" \
  -I"$repo_root/musl/pachaos/include" \
  "$repo_root/tests/lpr_drm_mapping_lease_unit.c" -o "$out/profile-unit"
"$out/profile-unit"
