#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
cd "$(dirname "$0")/.."
out=.artifacts/tests/kobox-drm-mapping-release
mkdir -p "$out"
python3 - "$out" <<'PY'
from pathlib import Path
import sys
parts=[]
for name,signature in [('gem/drm_mapping.c','static int release_system_memory_pages('),('boot/drm_file.c','int kobox_linux_drm_mapping_release(')]:
 src=Path('kobox2/linux-sandbox/kobox',name).read_text()
 start=src.index(signature);end=src.index('{',start)+1;depth=1
 while depth:
  depth+=(src[end]=='{')-(src[end]=='}');end+=1
 parts.append(src[start:end])
Path(sys.argv[1],'mapping_release_actual.inc').write_text('\n'.join(parts))
PY
clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
 -I "$out" tests/kobox_drm_mapping_release_unit.c -o "$out/unit"
"$out/unit"
