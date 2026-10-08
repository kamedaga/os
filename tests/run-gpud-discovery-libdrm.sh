#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
repo="$PWD"
toolchain="${MESA_TOOLCHAIN:-$repo/.artifacts/build/mesa-25.1.9/toolchain-b865abb753262990}"
bash tests/run-gpud-discovery-unit.sh
for profile in render primary; do
  fixture="$(mktemp -d "$repo/.artifacts/tests/gpud-discovery/fixture.XXXXXX")"
  args=()
  if [[ "$profile" == primary ]]; then args=(primary); fi
  .artifacts/tests/gpud-discovery/unit "$fixture" "${args[@]}"
  bwrap --unshare-user --uid 0 --gid 0 --ro-bind "$toolchain" / \
    --dev /dev --proc /proc --tmpfs /tmp \
    --ro-bind "$repo/tests/gpud_discovery_libdrm.c" /tmp/test.c \
    --bind "$fixture" /tmp/out \
    /usr/bin/gcc -std=c11 -Wall -Wextra -Werror -I/usr/include/libdrm \
    /tmp/test.c -Wl,--export-dynamic -ldrm -o /tmp/out/test
  bwrap --unshare-user --uid 0 --gid 0 --ro-bind "$toolchain" / \
    --ro-bind "$fixture/sys" /sys --dev /dev --ro-bind "$fixture/dev/dri" /dev/dri \
    --proc /proc --tmpfs /tmp --ro-bind "$fixture/test" /tmp/test /tmp/test "${args[@]}"
done
