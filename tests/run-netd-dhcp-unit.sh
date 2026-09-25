#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
artifact="$repo_root/.artifacts/tests/netd-dhcp-unit"
mkdir -p "$(dirname "$artifact")"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  "$repo_root/tests/netd_dhcp_unit.c" \
  "$repo_root/userland/netd/src/dhcp.c" \
  -o "$artifact"
"$artifact"
