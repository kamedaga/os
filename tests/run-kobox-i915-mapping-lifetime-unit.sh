#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
cd "$(dirname "$0")/.."
out=.artifacts/tests/kobox-i915-mapping-lifetime
mkdir -p "$out"
python3 - "$out" <<'PY'
from pathlib import Path
import sys
src=Path('kobox2/linux-sandbox/kobox/gem/i915_mapping.c').read_text()
start=src.index('struct mapped_bo {'); end=src.index('static int pin_owner(')
parts=[src[start:end]]
for signature in ('static int restore_bo(', 'static void free_bo(', 'static int quiesce(', 'static int release('):
 start=src.index(signature);brace=src.index('{',start);depth=1;end=brace+1
 while depth:
  depth+=(src[end]=='{')-(src[end]=='}');end+=1
 parts.append(src[start:end])
Path(sys.argv[1],'i915_mapping_lifetime_actual.inc').write_text('\n\n'.join(parts)+'\n')
PY
clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
 -I "$out" tests/kobox_i915_mapping_lifetime_unit.c -o "$out/unit"
"$out/unit"
