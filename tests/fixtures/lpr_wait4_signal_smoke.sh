#!/bin/bash

set -u

resource_probe()
{
    local stage=$1
    /bin/sync &
    local probe_pid=$!
    echo "LPR_WAIT4_SIGNAL_LPR_PROBE stage=$stage"
    wait "$probe_pid" || return 1
}

stop_timer()
{
    echo LPR_WAIT4_SIGNAL_TRAP
    if [[ -n ${timer_pid:-} ]]; then
        kill -TERM "$timer_pid" 2>/dev/null || true
        wait "$timer_pid" 2>/dev/null || true
    fi
}

target_log=/tmp/lpr-wait4-signal-target.log
rm -f -- "$target_log"
(
    timer_pid=
    trap stop_timer TERM
    resource_probe baseline || exit 1
    /bin/sleep 30 &
    timer_pid=$!
    echo "LPR_WAIT4_SIGNAL_READY child=$timer_pid shell=$BASHPID"
    wait "$timer_pid"
    wait_status=$?
    echo "LPR_WAIT4_SIGNAL_AFTER_WAIT status=$wait_status"
    resource_probe after-signal || exit 1
) >"$target_log" 2>&1 &
target_pid=$!

/bin/sleep 2
if ! kill -0 "$target_pid" 2>/dev/null; then
    echo LPR_WAIT4_SIGNAL_FAIL stage=target-missing
    exit 1
fi
echo "LPR_WAIT4_SIGNAL_TARGET_PRESENT pid=$target_pid"

kill -TERM "$target_pid"
kill_status=$?
echo "LPR_WAIT4_SIGNAL_KILL status=$kill_status"
if [[ $kill_status -ne 0 ]]; then
    echo LPR_WAIT4_SIGNAL_FAIL stage=kill
    exit 1
fi

wait "$target_pid"
wait_status=$?
/bin/cat "$target_log"
rm -f -- "$target_log"
echo "LPR_WAIT4_SIGNAL_PARENT_WAIT status=$wait_status"
if [[ $wait_status -ne 0 ]]; then
    echo LPR_WAIT4_SIGNAL_FAIL stage=parent-wait
    exit 1
fi

echo LPR_WAIT4_SIGNAL_DONE
