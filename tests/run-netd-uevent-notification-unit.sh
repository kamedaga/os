#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests"
mkdir -p "$out_dir"
/usr/bin/clang -std=c11 -Wall -Wextra -Werror -Wno-sign-compare \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -I"$repo_root/userland/netd/include" \
  -I"$repo_root/userland/netd/src" \
  -I"$repo_root/userland/libipc/include" \
  -I"$repo_root/userland/libpacha/include" \
  -I"$repo_root/_kobox/include" \
  "$repo_root/tests/netd_uevent_notification_unit.c" \
  -o "$out_dir/netd-uevent-notification-unit"
"$out_dir/netd-uevent-notification-unit"
