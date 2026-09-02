#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

.artifacts/bin/pacgo sync rootfs --force
.artifacts/bin/pacgo sync bootfs

.artifacts/bin/pacgo qemu-test \
  --timeout 100s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
  --send '/bin/bash /cmd/thunar_bringup.sh' \
  --expect 'THUNAR_BRINGUP_DBUS_PASS' \
  --expect 'THUNAR_BRINGUP_SWAY_PASS' \
  --expect 'THUNAR_BRINGUP_WAYLAND_PASS' \
  --expect 'THUNAR_BRINGUP_WINDOW_EVENT' \
  --expect 'pid_match=1 app_id_match=1' \
  --expect 'THUNAR_BRINGUP_PASS dbus=1 sway=1 wayland=1 window=1' \
  --expect 'THUNAR_BRINGUP_RESULT dbus=1 sway=1 wayland=1 window=1 first_error=none'
