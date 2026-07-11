#!/bin/sh

result=/tmp/mesa_inventory_result
status_file=/tmp/mesa_inventory_status
rm -f "$result" "$status_file"

run_stage() {
    stage="$1"
    limit="$2"
    variant="$3"
    printf 'MESA_RUN_STAGE stage=%s variant=%s timeout=%s\n' "$stage" "$variant" "$limit"
    if [ "$variant" = lp2 ]; then
        (LP_NUM_THREADS=2 LIBGL_DEBUG=verbose EGL_LOG_LEVEL=warning busybox timeout -s KILL "$limit" /cmd/lpr_mesa_inventory.elf "$stage" 2>&1; printf '%s\n' "$?" >"$status_file") | busybox tee -a "$result"
    else
        (LIBGL_DEBUG=verbose EGL_LOG_LEVEL=warning busybox timeout -s KILL "$limit" /cmd/lpr_mesa_inventory.elf "$stage" 2>&1; printf '%s\n' "$?" >"$status_file") | busybox tee -a "$result"
    fi
    status=$(busybox cat "$status_file")
    printf 'MESA_RUN_RESULT stage=%s variant=%s status=%s\n' "$stage" "$variant" "$status" | busybox tee -a "$result"
}

run_stage e 120 default
run_stage d 90 lp2
printf 'MESA_INVENTORY_DONE\n'
rm -f "$result" "$status_file"
exit 0
