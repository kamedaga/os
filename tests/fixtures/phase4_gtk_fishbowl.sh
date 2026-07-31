#!/bin/bash

set -u

runtime=/run/user/0
config=/cmd/phase4_gui_benchmark.conf
if [[ ${GTK_FISHBOWL_FULLSCREEN:-0} == 1 ]]; then
    config="$runtime/phase4_gtk_fishbowl.conf"
    rm -f "$config"
    while IFS= read -r line; do
        printf '%s\n' "$line"
    done </cmd/phase4_gui_benchmark.conf >"$config"
    printf '%s\n' \
        'for_window [app_id="gtk3-demo"] fullscreen enable, focus' \
        >>"$config"
fi

wait_tick()
{
    IFS= read -r -t "$1" -n 1 _ || true
}

pid_stopped()
{
    local pid=$1
    local state
    if ! kill -0 "$pid" 2>/dev/null; then
        return 0
    fi
    if [[ -r /proc/$pid/stat ]] &&
       read -r _ _ state _ <"/proc/$pid/stat" &&
       [[ $state == Z ]]; then
        return 0
    fi
    return 1
}

wait_for_socket()
{
    local sway_pid=$1
    local ticks=0
    while [[ $ticks -lt 1200 ]]; do
        shopt -s nullglob
        local sockets=("$runtime"/sway-ipc.*.sock)
        shopt -u nullglob
        if [[ ${#sockets[@]} -eq 1 ]]; then
            printf '%s\n' "${sockets[0]}"
            return 0
        fi
        pid_stopped "$sway_pid" && return 1
        wait_tick 0.1
        ticks=$((ticks + 1))
    done
    return 1
}

wait_for_tree_pid()
{
    local socket=$1
    local pid=$2
    local ticks=0
    while [[ $ticks -lt 90 ]]; do
        local tree
        tree=$(/usr/bin/swaymsg -s "$socket" -t get_tree 2>/dev/null) || true
        if [[ $tree =~ \"pid\"[[:space:]]*:[[:space:]]*$pid([,}]) ]]; then
            return 0
        fi
        pid_stopped "$pid" && return 1
        wait_tick 1
        ticks=$((ticks + 1))
    done
    return 1
}

stop_sway()
{
    local socket=$1
    local pid=$2
    /usr/bin/swaymsg -s "$socket" exit >/dev/null 2>&1 || true
    local ticks=0
    while ! pid_stopped "$pid" && [[ $ticks -lt 300 ]]; do
        wait_tick 0.1
        ticks=$((ticks + 1))
    done
    if ! pid_stopped "$pid"; then
        kill -TERM "$pid" 2>/dev/null || true
    fi
    wait "$pid" 2>/dev/null || true
}

if [[ ${XDG_RUNTIME_DIR:-} != "$runtime" ||
      ${LIBSEAT_BACKEND:-} != seatd ||
      ${SEATD_SOCK:-} != "$runtime/seatd.sock" ]]; then
    printf 'GTK_FISHBOWL_FAIL stage=session-environment\n'
    exit 1
fi

unset WAYLAND_DISPLAY SWAYSOCK
/usr/bin/sway -c "$config" &
sway_pid=$!
socket=$(wait_for_socket "$sway_pid") || {
    printf 'GTK_FISHBOWL_FAIL stage=sway-socket\n'
    kill -KILL "$sway_pid" 2>/dev/null || true
    exit 1
}

display=
ticks=0
while [[ $ticks -lt 1200 ]]; do
    shopt -s nullglob
    displays=("$runtime"/wayland-*)
    shopt -u nullglob
    for path in "${displays[@]}"; do
        [[ $path == *.lock ]] && continue
        display=${path##*/}
        break
    done
    [[ -n $display ]] && break
    wait_tick 0.1
    ticks=$((ticks + 1))
done
if [[ -z $display ]]; then
    printf 'GTK_FISHBOWL_FAIL stage=wayland-display\n'
    stop_sway "$socket" "$sway_pid"
    exit 1
fi

export WAYLAND_DISPLAY=$display
export SWAYSOCK=$socket
printf 'GTK_FISHBOWL_SWAY_READY display=%s\n' "$display"
GDK_BACKEND=wayland /usr/bin/gtk3-demo --run=fishbowl &
gtk_pid=$!
if ! wait_for_tree_pid "$socket" "$gtk_pid"; then
    printf 'GTK_FISHBOWL_FAIL stage=map\n'
    kill -KILL "$gtk_pid" 2>/dev/null || true
    stop_sway "$socket" "$sway_pid"
    exit 1
fi

wait_tick 3
printf 'GTK_FISHBOWL_BASELINE_BEGIN fullscreen=%s\n' \
    "${GTK_FISHBOWL_FULLSCREEN:-0}"
wait_tick 8
printf 'GTK_FISHBOWL_BASELINE_END\n'

printf 'GTK_FISHBOWL_MOUSE_BEGIN nominal_hz=%s delta=%s\n' \
    "${GTK_FISHBOWL_MOUSE_HZ:-1000}" \
    "${GTK_FISHBOWL_MOUSE_DELTA:-1}"
wait_tick 10
printf 'GTK_FISHBOWL_MOUSE_END\n'
wait_tick 1

kill -TERM "$gtk_pid" 2>/dev/null || true
ticks=0
while ! pid_stopped "$gtk_pid" && [[ $ticks -lt 300 ]]; do
    wait_tick 0.1
    ticks=$((ticks + 1))
done
if ! pid_stopped "$gtk_pid"; then
    printf 'GTK_FISHBOWL_FAIL stage=gtk-exit\n'
    kill -KILL "$gtk_pid" 2>/dev/null || true
    stop_sway "$socket" "$sway_pid"
    exit 1
fi
wait "$gtk_pid" 2>/dev/null || true
stop_sway "$socket" "$sway_pid"
printf 'GTK_FISHBOWL_PASS baseline_seconds=8 mouse_seconds=10 mouse_hz=%s delta=%s\n' \
    "${GTK_FISHBOWL_MOUSE_HZ:-1000}" \
    "${GTK_FISHBOWL_MOUSE_DELTA:-1}"
