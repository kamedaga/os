#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests"
mkdir -p "$out_dir"

cc="${CC:-cc}"
"$cc" \
  -std=c11 \
  -Wall \
  -Wextra \
  -Werror \
  -I "$repo_root/userland/libipc/include" \
  -I "$repo_root/userland/libpacha/include" \
  -I "$repo_root/userland/filed/include" \
  -I "$repo_root/kobox2/protocol/include" \
  -I "$repo_root/kobox2/protocol/generated/include" \
  -I "$repo_root/userland/netd/include" \
  -I "$repo_root/userland/unixd/include" \
  -I "$repo_root/musl/pachaos/include" \
  -I "$repo_root/userland/termd/include" \
  -I "$repo_root/userland/gpud/include" \
  -I "$repo_root/userland/lpr_supervisor/include" \
  -I "$repo_root/userland/personality/include" \
  "$repo_root/tests/userland_service_abi_layout.c" \
  -o "$out_dir/userland_service_abi_layout"

"$out_dir/userland_service_abi_layout"

# The native port uses the shared generated layout and exec codec, so no
# independent fixed-size wire mirror remains to keep in sync.
"$cc" -std=c11 -I "$repo_root/userland/libipc/include" \
  -I "$repo_root/userland/filed/include" \
  -I "$repo_root/kobox2/protocol/include" \
  -I "$repo_root/kobox2/protocol/generated/include" \
  "$repo_root/tests/native_exec_abi_layout.c" -o "$out_dir/native_exec_abi_layout"
"$out_dir/native_exec_abi_layout"

bash "$repo_root/tests/run-native-fs-port-unit.sh"
bash "$repo_root/tests/run-native-mmap-abi-unit.sh"
