#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
cd "$(dirname "$0")/.."
out=.artifacts/tests/kobox-drm-i915-create-ext
mkdir -p "$out"
python3 - "$out" <<'PY'
from pathlib import Path
import sys
s=Path('kobox2/linux-sandbox/kobox/boot/drm_file.c').read_text()
a=s.index('int kobox_linux_drm_i915_create_ext(');b=s.index('{',a)+1;depth=1
while depth:
 depth+=(s[b]=='{')-(s[b]=='}');b+=1
Path(sys.argv[1],'create_actual.inc').write_text(s[a:b])
PY
clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
 -D__EXPORTED_HEADERS__ -D__user= -I "$out" -I kobox2/linux-sandbox/include/uapi \
 tests/kobox_drm_i915_create_ext_unit.c -o "$out/unit"
"$out/unit" | tee "$out/result.log"
