#!/bin/sh

result=/tmp/drm_page_flip_result
status_file=/tmp/drm_page_flip_status
iters="${1:-20}"
frames="${2:-8}"
rm -f "$result" "$status_file"
(busybox timeout -s KILL 180 /cmd/lpr_drm_page_flip_smoke.elf "$iters" 2>&1; printf '%s\n' "$?" >"$status_file") | busybox tee "$result"
status=$(busybox cat "$status_file")
if [ "$status" -eq 0 ] && busybox grep -q '^FLIP_EVENT_PASS ' "$result"; then
    rm -f "$result" "$status_file"
else
    printf 'DRM_PAGE_FLIP_SMOKE_FAIL status=%s iterations=%s\n' "$status" "$iters"
    rm -f "$result" "$status_file"
    exit 1
fi

(LIBGL_DEBUG=verbose EGL_LOG_LEVEL=warning busybox timeout -s KILL 90 /cmd/lpr_mesa_cube_smoke.elf "$frames" 2>&1; printf '%s\n' "$?" >"$status_file") | busybox tee "$result"
status=$(busybox cat "$status_file")
if [ "$status" -eq 0 ] && busybox grep -q '^CUBE_ANIMATION_PASS ' "$result"; then
    rm -f "$result" "$status_file"
    printf 'DRM_PAGE_FLIP_SMOKE_DONE iterations=%s cube_frames=%s\n' "$iters" "$frames"
    exit 0
fi
printf 'DRM_PAGE_FLIP_CUBE_FAIL status=%s frames=%s\n' "$status" "$frames"
rm -f "$result" "$status_file"
exit 1
