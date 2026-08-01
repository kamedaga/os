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
  -I "$repo_root/userland/koboxd/include" \
  -I "$repo_root/userland/koboxd/src" \
  -I "$repo_root/userland/netd/include" \
  -I "$repo_root/userland/termd/include" \
  -I "$repo_root/userland/drmd/include" \
  -I "$repo_root/userland/lpr_supervisor/include" \
  -I "$repo_root/userland/personality/include" \
  "$repo_root/tests/userland_service_abi_layout.c" \
  -o "$out_dir/userland_service_abi_layout"

"$out_dir/userland_service_abi_layout"

extract_right_shift() {
  local path="$1"
  local symbol="$2"
  sed -nE \
    "s/^[[:space:]]*(#define[[:space:]]+)?${symbol}[[:space:]]*(=[[:space:]]*)?\\(?[[:space:]]*1[uU]?[[:space:]]*<<[[:space:]]*([0-9]+).*/\\3/p" \
    "$path"
}

common_flags="$repo_root/userland/filed/include/filed/flags.h"
arch_flags="$repo_root/musl/upstream/arch/pachaos/syscall_arch.h"
smoke_flags="$repo_root/musl/pachaos/smoke/libc_vfs_exec.c"
right_names=(LOOKUP READ WRITE EXEC STAT SETATTR GETDENTS CREATE REMOVE RENAME)

for expected_shift in "${!right_names[@]}"; do
  right_name="${right_names[$expected_shift]}"
  common_shift="$(extract_right_shift "$common_flags" "FILED_RIGHT_${right_name}")"
  arch_shift="$(extract_right_shift "$arch_flags" "PACHAOS_FILED_RIGHT_${right_name}")"
  smoke_shift="$(extract_right_shift "$smoke_flags" "PACHAOS_FILED_RIGHT_${right_name}")"
  if [[ "$common_shift" != "$expected_shift" ||
        "$arch_shift" != "$common_shift" ||
        "$smoke_shift" != "$common_shift" ]]; then
    echo "filed right ABI mismatch ${right_name}: expected=${expected_shift} common=${common_shift:-missing} arch=${arch_shift:-missing} smoke=${smoke_shift:-missing}" >&2
    exit 1
  fi
done
