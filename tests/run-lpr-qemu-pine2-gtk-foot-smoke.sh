#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

if [[ ${SKIP_SYNC:-0} != 1 ]]; then
  .artifacts/bin/pacgo sync rootfs
  .artifacts/bin/pacgo sync bootfs
fi

.artifacts/bin/pacgo qemu-test \
  --cpus "${PINE2_CPUS:-4}" \
  --timeout "${PINE2_TIMEOUT:-180}s" \
  --display gtk,gl=off \
  --graphics 2d \
  --input-profile keyboard-tablet \
  --send "PINE2_VIA_FOOT=1 PINE2_REPEAT=${PINE2_REPEAT:-5} bash /cmd/pine2_gtk_smoke.sh" \
  --expect "PINE2_GTK_SMOKE_PASS gtk=1 markdown=1 exit=0 via_foot=1 runs=${PINE2_REPEAT:-5}"

serial=.artifacts/serial-tty-test.log
if rg -q 'GENERAL PROTECTION|INVALID OPCODE|PAGE FAULT|USER fault' "$serial"; then
  rg -n 'GENERAL PROTECTION|INVALID OPCODE|PAGE FAULT|USER fault' "$serial" >&2
  exit 1
fi
