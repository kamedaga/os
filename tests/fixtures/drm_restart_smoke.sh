#!/bin/sh

result=/tmp/drm_restart_result
rm -f "$result"
iters="${1:-20}"

if busybox timeout -s KILL 60 /cmd/lpr_drm_restart_smoke.elf "$iters" >"$result" 2>&1; then
    busybox cat "$result"
else
    status="$?"
    busybox cat "$result"
    printf 'DRM_RESTART_DRIVER_FAIL status=%s\n' "$status"
    rm -f "$result"
    exit 1
fi

rm -f "$result"
printf 'DRM_RESTART_SMOKE_DONE iterations=%s delay_ms=0\n' "$iters"
exit 0
