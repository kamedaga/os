#!/bin/sh

result=/tmp/drm_prime_result
status_file=/tmp/drm_prime_status
rm -f "$result" "$status_file"
(busybox timeout -s KILL 30 /cmd/lpr_drm_prime_smoke.elf >"$result" 2>&1; printf '%s\n' "$?" >"$status_file")
status=$(busybox cat "$status_file")
busybox cat "$result"
printf 'DRM_PRIME_RESULT status=%s\n' "$status"
rm -f "$result" "$status_file"
exit 0
