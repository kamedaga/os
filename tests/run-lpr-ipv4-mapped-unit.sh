#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/lpr-ipv4-mapped
mkdir -p "$out"
python3 - "$out" <<'PY'
from pathlib import Path
import sys
s=Path('userland/personality/linux/runtime/lpr_socket.c').read_text();o=Path(sys.argv[1])
o.joinpath('mapped-types.inc').write_text(s[s.index('typedef struct lpr_linux_sockaddr_in {'):s.index('typedef struct lpr_linux_sockaddr_un {')])
o.joinpath('mapped-copy.inc').write_text(s[s.index('static int64_t lpr_socket_copy_sockaddr('):s.index('int64_t lpr_linux_socket(uint64_t')])
PY
clang -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -I"$out" tests/lpr_ipv4_mapped_unit.c -o "$out/unit"
"$out/unit"
