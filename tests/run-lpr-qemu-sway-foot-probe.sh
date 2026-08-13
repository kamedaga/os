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
  --send 'bash /cmd/sway_foot_probe.sh' \
  --expect 'P6_FOOT_PROBE_DONE' \
  --screendump-device pachagpu \
  --screendump-check 'P6_FOOT_SCREENSHOT_POINT@1000,744,8,8=#242424'

nonblack_pixels="$(python3 - <<'PY'
from pathlib import Path

data = Path('.artifacts/screendump-01.ppm').read_bytes().split(maxsplit=4)
pixels = data[4]
print(sum(
    1 for offset in range(0, len(pixels), 3)
    if pixels[offset:offset + 3] != b'\x00\x00\x00'
))
PY
)"
printf 'foot screendump non-black pixels: %s\n' "$nonblack_pixels"

if rg -Fq 'P6_FOOT_PTY_COMMAND_OK' .artifacts/console-tty-test.log &&
   rg -Fq 'P6_FOOT_ALIVE_AT_SCREENSHOT=1' .artifacts/console-tty-test.log &&
   rg -Fq 'P6_FOOT_CHILD_EXITED=1' .artifacts/console-tty-test.log &&
   rg -Fq 'P6_FOOT_EXIT=0' .artifacts/console-tty-test.log &&
   rg -Fq 'M51_LAUNCHER_CLIENT_EXIT=0' .artifacts/console-tty-test.log &&
   [[ "$nonblack_pixels" -gt 0 ]]; then
  printf 'foot upstream probe: PASS\n'
else
  printf 'foot upstream probe: FAIL\n' >&2
  exit 1
fi
