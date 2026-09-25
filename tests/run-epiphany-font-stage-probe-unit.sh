#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/epiphany-font-stage-probe
mkdir -p "$out"
flags=(-O2 -Wall -Wextra -Werror)
musl-gcc "${flags[@]}" -shared -fPIC -DPAGE_CYCLE_DIAG=1 -DPAGE_FONT_STAGE_DIAG=1 \
    tests/epiphany_load_calls.c -ldl -pthread -o "$out/probe.so"
musl-gcc "${flags[@]}" -shared -fPIC -DFONT_PROVIDER=1 \
    tests/epiphany_font_stage_probe_unit.c -o "$out/provider.so"
musl-gcc "${flags[@]}" tests/epiphany_font_stage_probe_unit.c \
    -L"$out" -l:provider.so -Wl,-rpath,"$PWD/$out" -o "$out/unit"
LD_PRELOAD="$PWD/$out/probe.so" "$out/unit" 2>"$out/probe.log"
python3 - "$out/probe.log" <<'PY'
import re, sys
from pathlib import Path
records = re.findall(r'PAGE_CYCLE_SUM .*call=(\w+) calls=(\d+) cycles=(\d+)',
                     Path(sys.argv[1]).read_text())
names = '''FcPatternCreate FcPatternDuplicate FcPatternDestroy FcPatternAddString
FcPatternAddLangSet FcFontSetCreate FcFontSetDestroy FcFontSetAdd FcLangSetCreate
FcLangSetDestroy FcLangSetAdd FcDefaultSubstitute FcConfigSubstitute FcPatternFilter
FcPatternRemoveSampled'''.split()
assert len(records) == len(names), records
assert {r[0] for r in records} == set(names), records
for name, count, cycles in records:
    assert int(count) == (2 if name == 'FcPatternDestroy' else 1), records
    assert int(cycles) > 0, records
print('font stage summary counts and cycles: PASS')
PY
