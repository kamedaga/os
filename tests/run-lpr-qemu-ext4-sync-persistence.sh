#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

if [[ "${SKIP_SYNC:-0}" != "1" ]]; then
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs
fi

# 2 ブート構成: write フェーズで書いて sync、read フェーズ (別ブート) で永続化を検証。
# 本体は rootfs 同梱の /cmd/ext4_w.sh, /cmd/ext4_r.sh (tests/fixtures/)。
# tty へは各ブートで起動 1 行だけ送る (python tty 直注入は不安定なため不使用)。

.artifacts/bin/pacgo qemu-test \
  --timeout 30s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=2' \
  --send 'bash /cmd/ext4_w.sh' \
  --expect 'EXT4W_FILE=OK' \
  --expect 'EXT4W_DIR=OK' \
  --expect 'EXT4W_DONE'

.artifacts/bin/pacgo qemu-test \
  --timeout 30s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=2' \
  --send 'bash /cmd/ext4_r.sh' \
  --expect 'EXT4R_FILE=OK' \
  --expect 'EXT4R_DIR=OK' \
  --expect 'EXT4R_CLEAN=OK' \
  --expect 'EXT4R_DONE'
