#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests/lpr-linux-process-compat"
mkdir -p "$out_dir"

/usr/bin/clang \
  -std=c11 -O2 -Wall -Wextra -Werror \
  -I"$repo_root/userland/libipc/include" \
  "$repo_root/userland/personality/linux/runtime/lpr_process/capability.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_process/compat.c" \
  "$repo_root/tests/lpr_linux_process_compat_unit.c" \
  -o "$out_dir/unit"

"$out_dir/unit"

for proc_value in \
  "$repo_root/userland/fixtures/base/proc-overflowuid" \
  "$repo_root/userland/fixtures/base/proc-overflowgid"; do
  if [[ "$(wc -c < "$proc_value")" -ne 6 ]] ||
     ! grep -qx '65534' "$proc_value"; then
    printf 'invalid bwrap proc compatibility value: %s\n' "$proc_value" >&2
    exit 1
  fi
done

printf 'lpr bwrap proc compatibility fixtures: PASS\n'
