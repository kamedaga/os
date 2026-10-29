#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
out="$root/.artifacts/netd-libuinet-reconfiguration"
mkdir -p "$out"
includes=(userland/netd/include userland/netd/src userland/gpud
  userland/filed/include userland/libfiled_client/include userland/libipc/include
  userland/libcapsule/include userland/libpacha/include kobox2/include
  kobox2/linux-sandbox/kobox kobox2/protocol/include kobox2/protocol/generated/include
  .artifacts/libuinet-pachaos/include .artifacts/mbedtls-pachaos/include)
args=()
for dir in "${includes[@]}"; do args+=("-I$root/$dir"); done
# The PIE archive contains GOTPCRELX pushes of function pointers. Keep those
# instructions intact: this host's GNU ld relaxes them into function calls.
clang -std=c11 -g -O1 -fno-omit-frame-pointer -no-pie -rdynamic -Wl,--no-relax -D_GNU_SOURCE -DNETD_WITH_LIBUINET \
  -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function "${args[@]}" \
  "$root/tests/netd_libuinet_reconfiguration.c" \
  "$root/userland/netd/src/network_config.c" "$root/userland/netd/src/link.c" \
  "$root/.artifacts/libuinet-pachaos/lib/libuinet.a" \
  "$root/.artifacts/mbedtls-pachaos/lib/libmbedtls-pachaos.a" \
  -o "$out/test"
sha256sum "$root/.artifacts/libuinet-pachaos/lib/libuinet.a" \
  "$root/.artifacts/mbedtls-pachaos/lib/libmbedtls-pachaos.a" \
  "$root/userland/netd/src/libuinet_backend.c" > "$out/inputs.sha256"
if (($#)); then
  timeout 40 "$out/test" "$@" 2>&1 | tee "$out/result.log"
else
  result=0
  for scenario in reconfigure tombstone-hup tombstone-hup-stream source-gate; do
    args=()
    if [[ "$scenario" != reconfigure ]]; then args=("--$scenario"); fi
    if timeout 40 "$out/test" "${args[@]}" 2>&1 | tee "$out/$scenario.log"; then
      echo "$scenario PASS"
    else
      echo "$scenario FAIL"
      result=1
    fi
  done
  exit "$result"
fi
