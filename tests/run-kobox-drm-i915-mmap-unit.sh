#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
cd "$(dirname "$0")/.."
out=.artifacts/tests/kobox-drm-i915-mmap
mkdir -p "$out"
python3 - "$out" <<'PY'
from pathlib import Path
import sys
source=Path('kobox2/linux-sandbox/kobox/boot/drm_file.c').read_text()
parts=[]
for signature in ('static bool valid_context(', 'static int check_owner(', 'static int private_ioctl(', 'int kobox_linux_drm_i915_map_pages('):
 start=source.index(signature);end=source.index('{',start)+1;depth=1
 while depth:
  depth+=(source[end]=='{')-(source[end]=='}');end+=1
 parts.append(source[start:end])
Path(sys.argv[1],'mmap_actual.inc').write_text('\n'.join(parts)+'\n')
PY
clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
 -D__EXPORTED_HEADERS__ -D__user= -I "$out" -I kobox2/linux-sandbox/include/uapi \
 tests/kobox_drm_i915_mmap_unit.c -o "$out/unit"
"$out/unit" | tee "$out/result.log"
