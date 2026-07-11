#!/bin/sh

result=/tmp/kms_modeset_result
status_file=/tmp/kms_modeset_status
rm -f "$result"
rm -f "$status_file"
(busybox timeout -s KILL 8 /cmd/lpr_kms_modeset_smoke.elf 2>&1; printf '%s\n' "$?" >"$status_file") | busybox tee "$result"
status=$(busybox cat "$status_file")
if [ "$status" -eq 0 ] && busybox grep -q '^KMS_MODESET_OK ' "$result"; then
    rm -f "$result" "$status_file"
    exit 0
fi
printf 'KMS_MODESET_FAIL status=%s\n' "$status"
rm -f "$result" "$status_file"
exit 1
