#!/bin/bash

set -u

runtime=/run/user/0
console=/dev/hvc0
action=${1:-}
app=${2:-}

now_ns()
{
    printf '%s000' "${EPOCHREALTIME/./}"
}

emit()
{
    printf '%s\n' "$*" >"$console"
}

pid_file="$runtime/key-phase-$app.pid"
done_file="$runtime/key-phase-$app.done"
log_file="$runtime/key-phase-$app.log"

case "$action:$app" in
launch:foot|launch:thunar)
    /bin/rm -f "$done_file" "$log_file"
    emit "KEY_PHASE_PROBE app=$app stage=launcher_start guest_ns=$(now_ns) launcher_pid=$$"
    if [[ $app == foot ]]; then
        /usr/bin/foot /bin/sh -c 'exec /bin/sleep 60' >"$log_file" 2>&1 &
    else
        GDK_BACKEND=wayland /usr/bin/thunar >"$log_file" 2>&1 &
    fi
    child_pid=$!
    printf '%s\n' "$child_pid" >"$pid_file"
    emit "KEY_PHASE_PROBE app=$app stage=process_created guest_ns=$(now_ns) pid=$child_pid"
    status=0
    wait "$child_pid" || status=$?
    emit "KEY_PHASE_PROBE app=$app stage=process_exit guest_ns=$(now_ns) pid=$child_pid status=$status"
    : >"$done_file"
    ;;
close:foot|close:thunar)
    pid=
    IFS= read -r pid <"$pid_file" 2>/dev/null || true
    emit "KEY_PHASE_CONTROL app=$app stage=close_start guest_ns=$(now_ns) pid=${pid:-0}"
    shopt -s nullglob
    sockets=("$runtime"/sway-ipc.*.sock)
    shopt -u nullglob
    if [[ -n $pid && ${#sockets[@]} -eq 1 ]]; then
        /usr/bin/swaymsg -s "${sockets[0]}" "[pid=\"$pid\"] kill" >/dev/null 2>&1 || true
    fi
    if [[ $app == thunar ]]; then
        GDK_BACKEND=wayland /usr/bin/thunar --quit >/dev/null 2>&1 || true
    fi
    for ((tick = 0; tick < 40; tick++)); do
        kill -0 "${pid:-0}" 2>/dev/null || break
        IFS= read -r -t 0.05 _ || true
    done
    if [[ -n $pid ]] && kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid" 2>/dev/null || true
    fi
    emit "KEY_PHASE_CONTROL app=$app stage=close_requested guest_ns=$(now_ns) pid=${pid:-0}"
    ;;
*)
    emit "KEY_PHASE_FAIL stage=invalid_probe action=$action app=$app"
    exit 2
    ;;
esac
