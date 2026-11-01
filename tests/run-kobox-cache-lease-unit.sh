#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/kobox-cache-lease
mkdir -p "$out"
python3 - "$out" <<'PY'
from pathlib import Path
import sys
out=Path(sys.argv[1]); root=Path('kobox2/linux-sandbox/kobox/memory')
src=(root/'cache_lease.c').read_text()
out.joinpath('cache_lease_actual.inc').write_text('\n'.join(line for line in src.splitlines() if not line.startswith('#include'))+'\n')
src=Path('kobox2/linux-sandbox/kobox/gem/i915_vmap_cache.c').read_text()
out.joinpath('i915_vmap_cache_actual.inc').write_text('\n'.join(line for line in src.splitlines() if not line.startswith('#include'))+'\n')
src=(root/'early_boot.c').read_text()
functions=[]
for signature in ('void kobox_cpa_wbinvd(', 'unsigned long kobox_memory_cache_alias_lock(',
                  'void kobox_memory_cache_alias_unlock(', 'int kobox_memory_cache_inspect(',
                  'int kobox_memory_cache_native(', 'static void protect_direct_range(',
                  'void flush_tlb_kernel_range('):
    start=src.index(signature);brace=src.index('{',start);depth=1;end=brace+1
    while depth:
        depth+=(src[end]=='{')-(src[end]=='}');end+=1
    functions.append(src[start:end])
out.joinpath('cache_bridge_actual.inc').write_text('\n\n'.join(functions)+'\n')
PY
clang -std=gnu11 -O1 -g -Wall -Wextra -Werror -fno-omit-frame-pointer \
  -fsanitize=address,undefined -pthread -I "$out" \
  tests/kobox_cache_lease_unit.c -o "$out/unit"
"$out/unit" | tee "$out/result.log"
sha256sum kobox2/linux-sandbox/kobox/gem/i915_vmap_cache.c \
  kobox2/linux-sandbox/kobox/memory/cache_lease.c \
  kobox2/linux-sandbox/kobox/memory/cache_lease.h \
  kobox2/linux-sandbox/kobox/memory/early_boot.c \
  tests/kobox_cache_lease_unit.c tests/run-kobox-cache-lease-unit.sh > "$out/inputs.sha256"
