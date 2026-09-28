#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
output="$repo_root/.artifacts/tests/gpud-inspect-policy-unit"
mkdir -p "$output"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
    -I"$repo_root/userland/gpud/include" \
    "$repo_root/tests/gpud_inspect_policy_unit.c" \
    "$repo_root/userland/gpud/inspect_policy.c" \
    -o "$output/unit"
"$output/unit"
