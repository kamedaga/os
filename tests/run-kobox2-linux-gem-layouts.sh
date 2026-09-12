#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
[[ $# == 4 ]] || {
  echo "Usage: $0 manifest module-inputs.json linux-boot-test linux-vm-client" >&2
  exit 2
}
manifest="$(realpath -e "$1")"
inputs="$(realpath -e "$2")"
boot_test="$(realpath -e "$3")"
client="$(realpath -e "$4")"
mkdir -p "$repo_root/.artifacts/tests/kobox2-linux-gem-layouts"
out="$(mktemp -d "$repo_root/.artifacts/tests/kobox2-linux-gem-layouts/run-XXXXXX")"
python3 "$repo_root/tests/check-kobox2-pacha-modules.py" \
  --manifest "$manifest" --inputs "$inputs" >"$out/inputs.sha256"
sha256sum "$boot_test" "$client" "$manifest" \
  "$repo_root/tests/kobox2_ascending_reservations.c" >>"$out/inputs.sha256"
source_for() {
  local entries source
  mapfile -t entries < <(awk -F= -v destination="$1" '$1 == destination { print $2 }' "$manifest")
  [[ "${#entries[@]}" == 1 ]] || return 1
  source="${entries[0]}"
  [[ "$source" == /* ]] || source="$(dirname "$manifest")/$source"
  realpath -e "$source"
}
core="$(source_for /srv/kobox2/core.so)"
modules=()
for module in i2c-core drm_panel_orientation_quirks drm drm_shmem_helper lifetime_test; do
  modules+=("$(source_for "/srv/kobox2/modules/$module.ko")")
done
"${CC:-cc}" -shared -fPIC -std=c11 -O2 -Wall -Wextra -Werror \
  "$repo_root/tests/kobox2_ascending_reservations.c" -o "$out/reservations.so"
sha256sum "$out/reservations.so" >>"$out/inputs.sha256"
for layout in default ascending; do
  preload=()
  [[ "$layout" != ascending ]] || preload=("LD_PRELOAD=$out/reservations.so")
  timeout 30s env -u LD_PRELOAD "${preload[@]}" "$boot_test" "$core" \
    --gem "$client" "${modules[@]}" >"$out/$layout.log" 2>&1 || {
      tail -30 "$out/$layout.log"
      echo "Linux GEM $layout failed: $out" >&2
      exit 1
    }
  rg -q 'permissions=6 loaded=5 unloaded=5 warnings=0 phase=6 result=0' "$out/$layout.log"
  rg -q 'GEM lifetime: phase=10 line=0 result=0' "$out/$layout.log"
done
printf 'Linux GEM default/ascending layouts PASS: %s\n' "$out"
