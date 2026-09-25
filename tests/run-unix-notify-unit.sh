#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests/unix-notify"
mkdir -p "$out_dir"
ulimit -c 0
clang -std=c11 -O2 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$repo_root/userland/unixd/include" \
  "$repo_root/userland/unixd/src/transport.c" \
  "$repo_root/userland/unixd/src/notify.c" \
  "$repo_root/userland/unixd/src/notify_client.c" \
  "$repo_root/tests/unix_notify_unit.c" -o "$out_dir/unit"
"$out_dir/unit"
