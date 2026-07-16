#!/bin/bash

set -u

runtime=/run/user/0

wait_tick()
{
    IFS= read -r -t "$1" -n 1 _ || true
}

if [[ ${1:-} == --shell || ${1:-} == --auto-exit-shell ]]; then
    stty_state=$(/bin/stty -a 2>/dev/null || true)
    icrnl=1
    icanon=1
    [[ " $stty_state " == *" -icrnl "* ]] && icrnl=0
    [[ " $stty_state " == *" -icanon "* ]] && icanon=0
    printf '\033]0;SWAY_FOOT_EXIT_SHELL shell_pid=%s foot_pid=%s icrnl=%s icanon=%s\007' \
        "$$" "$PPID" "$icrnl" "$icanon"
    if [[ ${1:-} == --auto-exit-shell ]]; then
        wait_tick 2
        exit 0
    fi
    export PS1='foot-exit# '
    exec /bin/bash --noprofile --norc -i
fi

wait_for_socket()
{
    local ticks=0
    while [[ $ticks -lt 1200 ]]; do
        shopt -s nullglob
        local sockets=("$runtime"/sway-ipc.*.sock)
        shopt -u nullglob
        if [[ ${#sockets[@]} -eq 1 ]]; then
            printf '%s\n' "${sockets[0]}"
            return 0
        fi
        wait_tick 0.1
        ticks=$((ticks + 1))
    done
    return 1
}

wait_for_foot_pids()
{
    local socket=$1
    local pattern='SWAY_FOOT_EXIT_SHELL shell_pid=([0-9]+) foot_pid=([0-9]+) icrnl=([01]) icanon=([01])'
    local ticks=0
    while [[ $ticks -lt 1200 ]]; do
        local tree
        tree=$(/usr/bin/swaymsg -s "$socket" -t get_tree 2>/dev/null || true)
        if [[ $tree =~ $pattern ]]; then
            printf '%s %s %s %s\n' \
                "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}" \
                "${BASH_REMATCH[3]}" "${BASH_REMATCH[4]}"
            return 0
        fi
        wait_tick 0.1
        ticks=$((ticks + 1))
    done
    return 1
}

if [[ ${XDG_RUNTIME_DIR:-} != "$runtime" ||
      ${LIBSEAT_BACKEND:-} != seatd ||
      ${SEATD_SOCK:-} != "$runtime/seatd.sock" ]]; then
    printf 'SWAY_FOOT_EXIT_RESULT setup=bad-environment shell_alive=1 foot_alive=1\n'
    exit 1
fi

/usr/bin/sway -c /cmd/sway_foot_exit_acceptance.conf >/dev/null 2>&1 &
sway_pid=$!
socket=$(wait_for_socket) || {
    printf 'SWAY_FOOT_EXIT_RESULT setup=no-socket shell_alive=1 foot_alive=1\n'
    exit 1
}
foot_pids=$(wait_for_foot_pids "$socket") || {
    printf 'SWAY_FOOT_EXIT_RESULT setup=no-foot shell_alive=1 foot_alive=1\n'
    exit 1
}
shell_pid=${foot_pids%% *}
foot_state=${foot_pids#* }
foot_pid=${foot_state%% *}
termios_state=${foot_state#* }
icrnl=${termios_state%% *}
icanon=${termios_state#* }

printf 'SWAY_FOOT_EXIT_CHILD_READY shell_pid=%s foot_pid=%s icrnl=%s icanon=%s\n' \
    "$shell_pid" "$foot_pid" "$icrnl" "$icanon"

ticks=0
shell_alive=1
foot_alive=1
while [[ $ticks -lt 100 ]]; do
    shell_alive=0
    foot_alive=0
    kill -0 "$shell_pid" 2>/dev/null && shell_alive=1
    kill -0 "$foot_pid" 2>/dev/null && foot_alive=1
    if [[ $shell_alive -eq 0 && $foot_alive -eq 0 ]]; then
        break
    fi
    wait_tick 0.1
    ticks=$((ticks + 1))
done

printf 'SWAY_FOOT_EXIT_RESULT setup=ok shell_alive=%s foot_alive=%s\n' \
    "$shell_alive" "$foot_alive"

if [[ $foot_alive -ne 0 ]]; then
    kill -TERM "$foot_pid" 2>/dev/null || true
fi
/usr/bin/swaymsg -s "$socket" exit >/dev/null 2>&1 || true
wait "$sway_pid" 2>/dev/null || true

[[ $shell_alive -eq 0 && $foot_alive -eq 0 ]]
