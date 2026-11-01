#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/kobox-drm-i915-query
mkdir -p "$out"
python3 - "$out" <<'PY'
from pathlib import Path
import sys
source=Path('kobox2/linux-sandbox/kobox/boot/drm_file.c').read_text()
functions=[]
for signature in ('static bool valid_context(', 'static int check_owner(',
                  'int kobox_linux_drm_i915_query('):
    start=source.index(signature); brace=source.index('{',start); depth=1; end=brace+1
    while depth:
        depth += (source[end]=='{')-(source[end]=='}'); end+=1
    functions.append(source[start:end])
Path(sys.argv[1],'drm_i915_query_actual.inc').write_text('\n\n'.join(functions)+'\n')
PY
clang -std=c11 -O1 -g -Wall -Wextra -Werror -fno-omit-frame-pointer \
  -fsanitize=address,undefined -D__EXPORTED_HEADERS__ -D__user= -I "$out" -I kobox2/linux-sandbox/include/uapi \
  -I kobox2/protocol/include -I kobox2/protocol/generated/include \
  tests/kobox_drm_i915_query_unit.c -o "$out/unit"
"$out/unit" | tee "$out/result.log"
sha256sum kobox2/linux-sandbox/kobox/boot/drm_file.c \
  kobox2/linux-sandbox/kobox/boot/drm_file.h \
  tests/kobox_drm_i915_query_unit.c tests/run-kobox-drm-i915-query-unit.sh > "$out/inputs.sha256"
