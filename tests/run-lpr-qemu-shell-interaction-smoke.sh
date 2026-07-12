#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

if [[ "${SKIP_SYNC:-0}" != "1" ]]; then
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs
fi

expects=(
  --expect 'SHELL_INTERACTION_START'
  --expect 'SHELL_ABA_BUSYBOX_1=OK'
  --expect 'SHELL_ABA_CD=OK'
  --expect 'SHELL_ABA_BUSYBOX_2=OK'
  --expect 'SHELL_ABA_LS=OK'
  --expect 'SHELL_ABA_BUSYBOX_3=OK'
  --expect 'SHELL_ABA_DONE'
  --expect 'SHELL_HISTORY_CAT_1=OK'
  --expect 'SHELL_HISTORY_PWD_1=OK'
  --expect 'SHELL_HISTORY_LS_1=OK'
  --expect 'SHELL_HISTORY_BUSYBOX_1=OK'
  --expect 'SHELL_HISTORY_CAT_2=OK'
  --expect 'SHELL_HISTORY_PWD_2=OK'
  --expect 'SHELL_HISTORY_LS_2=OK'
  --expect 'SHELL_HISTORY_BUSYBOX_2=OK'
  --expect 'SHELL_NOARG_BUSYBOX=OK'
  --expect 'SHELL_NOARG_LS=OK'
  --expect 'SHELL_NOARG_TRUE=OK'
  --expect 'SHELL_PATH_BUSYBOX=OK'
  --expect 'SHELL_ABSOLUTE_BUSYBOX=OK'
  --expect 'SHELL_RELATIVE_BUSYBOX=OK'
  --expect 'SHELL_CD_GLOB=OK'
  --expect 'SHELL_REDIRECTION=OK'
  --expect 'SHELL_EXIT_STATUS=OK'
  --expect 'SHELL_SHEBANG=OK'
  --expect 'SHELL_ENVIRONMENT=OK'
  --expect 'SHELL_REPEAT_BUSYBOX=OK'
  --expect 'SHELL_REPEAT_CAT=OK'
  --expect 'SHELL_REPEAT_PWD=OK'
  --expect 'SHELL_REPEAT_LS=OK'
  --expect 'SHELL_LS_THEN_EXEC=OK'
  --expect 'SHELL_LARGE_LS_THEN_EXEC=OK'
  --expect 'SHELL_INTERACTION_DONE failures=0'
)

.artifacts/bin/pacgo qemu-test \
  --timeout "${SHELL_INTERACTION_TIMEOUT:-120s}" \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=2' \
  --send '. /cmd/shell_interaction_smoke.sh' \
  "${expects[@]}"
