#!/bin/sh

result=/tmp/drm_card0_result
rm -f "$result"

if busybox timeout -s KILL 20 /cmd/lpr_drm_card0_smoke.elf --render >"$result" 2>&1; then
    status=0
else
    status=$?
fi

if [ "$status" -eq 0 ] &&
    grep -q '^DRM_RENDER_OK name=virtio_gpu version=.* prime=3 prime_share=1 virgl=1 caps=1 syncobj=1 context_gate=1 context_mode=.* exec=1 resource=1 mmap=1 poll=1 read=1 dup=1 fork_lease=1 last_close=1 client_kill=1 reopen_client=1$' "$result"; then
    cat "$result"
    rm -f "$result"
    exit 0
fi

cat "$result"
printf 'DRM_RENDER_FAIL status=%s\n' "$status"
rm -f "$result"
exit 1
