#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .artifacts/tests
# Compile the actual production batch/completion body with a kernel boundary
# double. No copied implementation can silently diverge from lpr_socket.c.
python3 - <<'PY'
from pathlib import Path
s=Path('userland/personality/linux/runtime/lpr_socket.c').read_text()
a=s.index('struct lpr_pipe_poll_batch {');b=s.index('};',a)+2
c=s.index('static int64_t lpr_pipe_poll_batch_complete(');d=s.index('{',c);depth=1;e=d+1
while depth:
 depth+=(s[e]=='{')-(s[e]=='}');e+=1
f=s.index('typedef struct lpr_linux_pollfd {');g=s.index('} lpr_linux_pollfd_t;',f)+len('} lpr_linux_pollfd_t;')
macros='\n'.join(line for line in s.splitlines() if line.startswith('#define LPR_LINUX_POLL'))
Path('.artifacts/tests/lpr-poll-batch-slice.h').write_text(macros+'\n'+s[f:g]+'\n'+s[a:b]+'\n'+s[c:e]+'\n')
PY
includes=(-Iuserland -Imusl/pachaos/include -Ikobox2/include -Ikobox2/protocol/include -Ikobox2/protocol/generated/include)
for module in libipc libpacha personality filed termd gpud inputd netd unixd lpr_supervisor; do includes+=("-Iuserland/$module/include"); done
clang -std=c11 -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
 -ffunction-sections -fdata-sections "${includes[@]}" tests/lpr_poll_batch_prefix_unit.c \
 -Wl,--gc-sections -o .artifacts/tests/lpr-poll-batch-prefix-unit
.artifacts/tests/lpr-poll-batch-prefix-unit
