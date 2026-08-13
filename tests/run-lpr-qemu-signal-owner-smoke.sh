#!/usr/bin/env bash
# Regression smoke (Phase 6 Step 1): a process-directed signal is delivered to
# the registered signal-owner thread (the main/event-loop thread), not to an
# arbitrary worker that happens to block first.  Green since the owner-preferred
# deliverSignal fix in kernel/src/scheduler_connection.zig.
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

if [[ "${SKIP_SYNC:-0}" != "1" ]]; then
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs
fi

pkill -9 qemu-system-x86 2>/dev/null || true
.artifacts/bin/pacgo qemu-test \
  --timeout 30s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
  --send '/cmd/lpr_signal_owner_red.elf' \
  --expect 'SIGNAL_OWNER_START' \
  --expect 'SIGNAL_OWNER_WORKER_BLOCKING' \
  --expect 'SIGNAL_OWNER_EVENT_LOOP_BLOCKING' \
  --expect 'SIGNAL_OWNER_PROCESS_SIGNAL_SENT' \
  --expect 'SIGNAL_OWNER_HANDLER=OWNER' \
  --expect 'SIGNAL_OWNER_DELIVERY=OK' \
  --expect 'SIGNAL_OWNER_DONE'
