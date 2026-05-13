#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
cd "$REPO_ROOT"

LOOPS="${PIPE_STRESS_LOOPS:-100}"
OUT_DIR="${PIPE_STRESS_OUT:-.artifacts/pipe-stress}"

if ! command -v python3 >/dev/null 2>&1; then
  echo "missing python3"
  exit 1
fi

if [ "$(uname -s)" = "Linux" ] && [ ! -r /usr/share/OVMF/OVMF_CODE_4M.fd ]; then
  echo "missing /usr/share/OVMF/OVMF_CODE_4M.fd"
  exit 1
fi

if [ ! -f .artifacts/disk.img ]; then
  echo "missing .artifacts/disk.img"
  echo "run pactl setup first"
  exit 1
fi

exec python3 tools/pipe_stress.py --loops "$LOOPS" --out "$OUT_DIR" "$@"
