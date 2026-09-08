#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"
mkdir -p .artifacts/tests/lpr-pipe-poll
/usr/bin/clang -std=c11 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -Iuserland/libipc/include -Iuserland/libpacha/include \
  -Iuserland/personality/include -Imusl/pachaos/include \
  -Iuserland/daemons/common/include -Iuserland/filed/include \
  -Iuserland/termd/include -Iuserland/drmd/include -Iuserland/inputd/include \
  -Iuserland/netd/include -Iuserland/lpr_supervisor/include -I_kobox/include \
  -Iuserland/unixd/include \
  tests/lpr_pipe_poll_unit.c -o .artifacts/tests/lpr-pipe-poll/unit
.artifacts/tests/lpr-pipe-poll/unit
