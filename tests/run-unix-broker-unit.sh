#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests/unix-broker"
mkdir -p "$out_dir"
clang -std=c11 -O2 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$repo_root/userland/unixd/include" -I"$repo_root/userland/unixd/src" \
  "$repo_root/userland/unixd/src/broker.c" \
  "$repo_root/userland/unixd/src/rights.c" \
  "$repo_root/userland/unixd/src/transport.c" \
  "$repo_root/userland/unixd/src/notify.c" \
  "$repo_root/tests/unix_broker_unit.c" -o "$out_dir/unit"
"$out_dir/unit"
