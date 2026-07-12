#!/bin/bash

if [ "${M53B_MONITOR:-1}" -eq 1 ]; then
    export M53_MONITOR_IDLE_MS=120000
    /cmd/lpr_udev_discovery.elf &
    /cmd/busybox sleep 1
fi

SECONDS=0
. /cmd/shell_interaction_smoke.sh
status=$?
echo "M53B_SHELL_SECONDS=$SECONDS"
echo "M53B_MONITOR_SHELL_STATUS=$status"
exit "$status"
