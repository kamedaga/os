#!/bin/sh

echo ASYNC_SIGNAL_START

busybox timeout -s KILL 15 /cmd/lpr_async_signal_smoke.elf suite
status=$?
if [ "$status" -ne 0 ]; then
    echo "ASYNC_SIGNAL_SUITE=BAD status=$status"
    echo "ASYNC_SIGNAL_DONE failures=1"
    exit 1
fi

/bin/sync
echo "ASYNC_SIGNAL_DONE failures=0"
