#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/kobox-drm-i915-context
mkdir -p "$out"
python3 - "$out" <<'PY'
from pathlib import Path
import sys
source=Path('kobox2/linux-sandbox/kobox/boot/drm_file.c').read_text()
functions=[]
for signature in ('static bool valid_context(', 'static int check_owner(',
                  'static int private_ioctl(', 'int kobox_linux_drm_i915_context('):
    start=source.index(signature); brace=source.index('{',start); depth=1; end=brace+1
    while depth:
        depth += (source[end]=='{')-(source[end]=='}'); end+=1
    functions.append(source[start:end])
Path(sys.argv[1],'drm_i915_context_actual.inc').write_text('\n\n'.join(functions)+'\n')
PY
clang -std=c11 -O1 -g -Wall -Wextra -Werror -fno-omit-frame-pointer \
  -fsanitize=address,undefined -D__EXPORTED_HEADERS__ -D__user= \
  -I "$out" -I kobox2/linux-sandbox/include/uapi \
  -I kobox2/protocol/include -I kobox2/protocol/generated/include \
  tests/kobox_drm_i915_context_unit.c -o "$out/unit"
"$out/unit" | tee "$out/result.log"
sha256sum kobox2/linux-sandbox/kobox/boot/drm_file.c \
  kobox2/linux-sandbox/kobox/boot/drm_file.h \
  kobox2/protocol/include/kobox2/i915_context_shape.h \
  tests/kobox_drm_i915_context_unit.c tests/run-kobox-drm-i915-context-unit.sh > "$out/inputs.sha256"
