#!/bin/sh

result=/tmp/drm_card0_result
rm -f "$result"

if busybox timeout -s KILL 4 /cmd/lpr_drm_card0_smoke.elf >"$result" 2>&1; then
    status=0
else
    status=$?
fi

if [ "$status" -eq 0 ] && grep -q '^DRM_CARD0_OK name=.* version=.* dumb=1$' "$result"; then
    cat "$result"
    rm -f "$result"
    exit 0
fi

cat "$result"
printf 'DRM_CARD0_FAIL status=%s\n' "$status"
rm -f "$result"
exit 1
