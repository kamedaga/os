#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"
if [[ "${SKIP_SYNC:-0}" != "1" ]]; then
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs
fi
pkill -9 qemu-system-x86 2>/dev/null || true
sleep 1
.artifacts/bin/pacgo qemu-test \
  --timeout 240s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
  --send 'bash /cmd/sway_wayland_info_probe.sh' \
  --expect 'P6_WAYLAND_INFO_PROBE_DONE'

if rg -Fq 'P6_WAYLAND_INFO_EXEC path=/usr/bin/wayland-info' .artifacts/console-tty-test.log &&
   rg -Fq 'wl_compositor' .artifacts/console-tty-test.log &&
   rg -Fq 'wl_shm' .artifacts/console-tty-test.log &&
   rg -Fq 'xdg_wm_base' .artifacts/console-tty-test.log &&
   rg -Fq 'wl_seat' .artifacts/console-tty-test.log &&
   rg -Fq 'M51_LAUNCHER_CLIENT_EXIT=0' .artifacts/console-tty-test.log; then
  printf 'wayland-info upstream probe: PASS\n'
else
  printf 'wayland-info upstream probe: FAIL\n' >&2
  exit 1
fi
