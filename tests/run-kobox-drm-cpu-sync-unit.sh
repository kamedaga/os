#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/kobox-drm-cpu-sync
mkdir -p "$out"
python3 - "$out" <<'PY'
from pathlib import Path
import sys
source=Path('kobox2/linux-sandbox/kobox/boot/drm_service.c').read_text()
functions=[]
for signature in ('static int check_owner(', 'int kobox_linux_drm_service_prime_cpu_sync('):
    start=source.index(signature); brace=source.index('{',start); depth=1; end=brace+1
    while depth:
        depth += (source[end]=='{')-(source[end]=='}'); end+=1
    functions.append(source[start:end])
Path(sys.argv[1],'drm_cpu_sync_actual.inc').write_text('\n\n'.join(functions)+'\n')
PY
clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$out" tests/kobox_drm_cpu_sync_unit.c -o "$out/unit"
"$out/unit"
