#!/bin/bash

set -u

runtime=/run/user/0

wait_tick()
{
    IFS= read -r -t "$1" -n 1 _ || true
}

if [[ ${1:-} == --gtk-child ]]; then
    printf '%s %s\n' "$$" "$PPID" >"$runtime/sway-foot-gtk-pids"
    printf '\033]0;SWAY_FOOT_GTK_CHILD gtk_pid=%s foot_pid=%s\007' "$$" "$PPID"
    /bin/sleep 1
    printf 'exec\n' >"$runtime/sway-foot-gtk-exec"
    exec env GDK_BACKEND=wayland /usr/bin/gtk3-demo
fi

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

wait_for_gtk_pids()
{
    local pid_file="$runtime/sway-foot-gtk-pids"
    local ticks=0
    while [[ $ticks -lt 1200 ]]; do
        local pids=
        if [[ -r $pid_file ]]; then
            pids=$(<"$pid_file")
        fi
        if [[ $pids =~ ^[0-9]+[[:space:]][0-9]+$ ]]; then
            printf '%s\n' "$pids"
            return 0
        fi
        wait_tick 0.1
        ticks=$((ticks + 1))
    done
    return 1
}

wait_for_gtk_exec()
{
    local pid=$1
    local ticks=0
    while [[ $ticks -lt 100 ]]; do
        if [[ -e $runtime/sway-foot-gtk-exec ]]; then
            # Do not poll swaymsg here: every query is another Linux exec and
            # turns this lifecycle test into an unrelated process-table stress
            # test. Give GTK's dynamic loader time to finish before SIGHUP.
            wait_tick 5
            pid_is_running "$pid"
            return
        fi
        wait_tick 0.1
        ticks=$((ticks + 1))
    done
    return 1
}

wait_for_processes_gone()
{
    local first_pid=$1
    local second_pid=$2
    local ticks=0
    while [[ $ticks -lt 100 ]]; do
        if ! pid_is_running "$first_pid" && ! pid_is_running "$second_pid"; then
            return 0
        fi
        wait_tick 0.1
        ticks=$((ticks + 1))
    done
    return 1
}

pid_is_running()
{
    local pid=$1
    local state
    kill -0 "$pid" 2>/dev/null || return 1
    if [[ -r /proc/$pid/stat ]] &&
       read -r _ _ state _ <"/proc/$pid/stat" &&
       [[ $state == Z ]]; then
        return 1
    fi
    return 0
}

if [[ ${1:-} == --gtk-hangup-test ]]; then
    /bin/rm -f "$runtime/sway-foot-gtk-pids" "$runtime/sway-foot-gtk-exec"
    /usr/bin/sway -c /cmd/sway_foot_gtk_hangup.conf >/dev/null 2>&1 &
    sway_pid=$!
    socket=$(wait_for_socket) || {
        printf 'SWAY_FOOT_GTK_HANGUP_RESULT setup=no-socket gtk_alive=1 foot_alive=1\n'
        exit 1
    }
    printf 'SWAY_FOOT_GTK_HANGUP_STAGE socket=ready\n'
    gtk_pids=$(wait_for_gtk_pids) || {
        printf 'SWAY_FOOT_GTK_HANGUP_RESULT setup=no-foot gtk_alive=1 foot_alive=1\n'
        exit 1
    }
    printf 'SWAY_FOOT_GTK_HANGUP_STAGE child=ready pids=%s\n' "$gtk_pids"
    gtk_pid=${gtk_pids%% *}
    foot_pid=${gtk_pids#* }
    wait_for_gtk_exec "$gtk_pid" || {
        printf 'SWAY_FOOT_GTK_HANGUP_RESULT setup=no-gtk gtk_alive=1 foot_alive=1\n'
        exit 1
    }
    printf 'SWAY_FOOT_GTK_HANGUP_STAGE gtk=exec-alive\n'
    printf 'SWAY_FOOT_GTK_HANGUP_READY gtk_pid=%s foot_pid=%s\n' "$gtk_pid" "$foot_pid"
    foot_window=1
    if /usr/bin/swaymsg -s "$socket" '[app_id="foot"] kill' >/dev/null 2>&1; then
        foot_window=0
    fi
    wait_for_processes_gone "$gtk_pid" "$foot_pid" || true
    gtk_alive=0
    foot_alive=0
    pid_is_running "$gtk_pid" && gtk_alive=1
    pid_is_running "$foot_pid" && foot_alive=1
    printf 'SWAY_FOOT_GTK_HANGUP_RESULT setup=ok gtk_alive=%s foot_window=%s foot_alive=%s\n' \
        "$gtk_alive" "$foot_window" "$foot_alive"
    [[ $gtk_alive -ne 0 ]] && kill -KILL "$gtk_pid" 2>/dev/null || true
    [[ $foot_alive -ne 0 ]] && kill -KILL "$foot_pid" 2>/dev/null || true
    /usr/bin/swaymsg -s "$socket" exit >/dev/null 2>&1 || true
    wait "$sway_pid" 2>/dev/null || true
    /bin/rm -f "$runtime/sway-foot-gtk-pids" "$runtime/sway-foot-gtk-exec"
    [[ $gtk_alive -eq 0 && $foot_window -eq 0 ]]
    exit
fi

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
