#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
r=.artifacts/tests/gpud-drm-fairness
mkdir -p "$r"
python3 - <<'PY'
from pathlib import Path
s=Path('userland/gpud/drm_service.c').read_text();a=s.index('int gpud_drm_service_receive(');b=s.index('\nint gpud_drm_service_reject_unavailable(',a)
Path('.artifacts/tests/gpud-drm-fairness/receive-actual.inc').write_text(s[a:b])
PY
clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$r" tests/gpud_drm_fairness_unit.c -o "$r/unit"
"$r/unit"
