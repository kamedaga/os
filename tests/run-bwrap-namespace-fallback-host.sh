#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests/bwrap-namespace-fallback"
bwrap_bin="${BWRAP_BIN:-/usr/bin/bwrap}"
mkdir -p "$out_dir"

if [[ ! -x "$bwrap_bin" ]]; then
  printf 'bwrap namespace fallback host: SKIP (%s missing)\n' "$bwrap_bin"
  exit 0
fi

/usr/bin/clang \
  -std=c11 -O2 -Wall -Wextra -Werror \
  "$repo_root/tests/bwrap_namespace_block_probe.c" \
  -o "$out_dir/probe"

set +e
output="$(
  "$out_dir/probe" \
    "$bwrap_bin" \
    --unshare-all \
    --die-with-parent \
    --chdir / \
    --ro-bind / / \
    -- /bin/true \
    2>&1
)"
status=$?
set -e

if [[ $status -eq 0 ]]; then
  printf 'FAIL: bwrap unexpectedly created namespaces\n' >&2
  exit 1
fi
if [[ "$output" != *"Creating new namespace failed"* &&
      "$output" != *"No permissions to create a new namespace"* &&
      "$output" != *"No permissions to create new namespace"* ]]; then
  printf 'FAIL: glycin would not recognize bwrap failure: %s\n' "$output" >&2
  exit 1
fi

printf 'bwrap namespace fallback host: PASS (status=%s)\n' "$status"
