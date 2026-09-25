#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
artifact="$repo_root/.artifacts/tests/netd-link-unit"
mkdir -p "$(dirname "$artifact")"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$repo_root/userland/netd/include" \
  "$repo_root/tests/netd_link_unit.c" \
  "$repo_root/userland/netd/src/link.c" \
  "$repo_root/userland/netd/src/network_config.c" \
  -o "$artifact"
"$artifact" "$repo_root/pack/live-bootfs/network.conf"
