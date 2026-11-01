#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/lpr-select-restart
mkdir -p "$out"
# Test the production function verbatim; other socket backends are mocked.
python3 - "$out/select-wait.inc" <<'PY'
from pathlib import Path
import sys
s=Path('userland/personality/linux/runtime/lpr_socket.c').read_text()
a=s.index('static int64_t lpr_linux_select_wait(')
b=s.index('\nint64_t lpr_linux_select(',a)
Path(sys.argv[1]).write_text(s[a:b])
PY
clang -std=c11 -O2 -fsanitize=address,undefined -I"$out" tests/lpr_select_restart_unit.c -o "$out/unit"
"$out/unit"
