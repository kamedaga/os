#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests/lpr-proc-maps"
mkdir -p "$out_dir"
"${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=all \
 -I "$repo_root/userland/libpacha/include" "$repo_root/tests/lpr_proc_maps_unit.c" -o "$out_dir/unit"
"$out_dir/unit"
