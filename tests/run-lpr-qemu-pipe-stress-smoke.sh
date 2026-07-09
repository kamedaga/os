#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

if [[ "${SKIP_SYNC:-0}" != "1" ]]; then
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs
fi

# ハーネス設計 (T3.0):
# - テスト本体は rootfs 同梱の /cmd/pipe_stress.sh。tty へは起動 1 行だけ送る
#   (tty 入力遅延で長いコマンドが不安定になるため)
# - 期待マーカーは送信文字列に含めない (tty エコー誤マッチ防止)
# - PIPE_STRESS_ITERS: 反復回数 (既定 5)。診断は 1 を推奨
# - PIPE_STRESS_EXPECT_DONE_ONLY=1: 完走マーカーのみ検証 (診断モード)
# ブートは約 2 秒。happy path は数秒で完走するため timeout は短くてよい。

iters="${PIPE_STRESS_ITERS:-5}"

expects=(--expect 'PIPE_STRESS_DONE')
if [[ "${PIPE_STRESS_EXPECT_DONE_ONLY:-0}" != "1" ]]; then
  for ((i=1; i<=iters; i++)); do
    for c in 1 2 3 4 5 6 7 8; do
      expects+=(--expect "I${i}_CASE${c}=OK")
    done
  done
fi

.artifacts/bin/pacgo qemu-test \
  --timeout "${PIPE_STRESS_TIMEOUT:-60s}" \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=2' \
  --send "bash /cmd/pipe_stress.sh ${iters}" \
  "${expects[@]}"
