#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

if [[ "${SKIP_SYNC:-0}" != "1" ]]; then
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs
fi

# 本体は rootfs 同梱の /cmd/gnu_smoke.sh (tests/fixtures/gnu_coreutils_smoke.sh)。
# tty へは起動 1 行だけ送る (python tty 直注入は不安定なため不使用)。

.artifacts/bin/pacgo qemu-test \
  --timeout 30s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
  --send 'bash /cmd/gnu_smoke.sh' \
  --expect 'GNUCU_GETDENTS_CWD=OK' \
  --expect 'GNUCU_GETDENTS_MULTIBATCH=OK' \
  --expect 'GNUCU_CASE1=OK' \
  --expect 'GNUCU_CASE2=OK' \
  --expect 'GNUCU_CASE3=OK' \
  --expect 'GNUCU_CASE4=OK' \
  --expect 'GNUCU_CASE5=OK' \
  --expect 'GNUCU_CASE6=OK' \
  --expect 'GNUCU_CASE7=OK' \
  --expect 'GNUCU_LOADER_REPEAT=OK' \
  --expect 'GNUCU_DONE'
