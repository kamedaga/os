#!/bin/bash

set -u

runtime=/run/user/0
config="$runtime/thunar-bringup.conf"
dbus_socket="$runtime/dbus-session-bus"
dbus_log="$runtime/thunar-dbus.log"
dbus_probe_log="$runtime/thunar-dbus-probe.log"
sway_log="$runtime/thunar-sway.log"
thunar_log="$runtime/thunar-app.log"
event_log="$runtime/thunar-window-events.log"
wait_limit=600

dbus_pid=
sway_pid=
thunar_pid=
event_pid=
sway_socket=
wayland_display=

wait_pipe_start()
{
    coproc THUNAR_WAIT_PIPE { while IFS= read -r _; do :; done; }
    wait_fd=${THUNAR_WAIT_PIPE[0]}
    wait_pid=$THUNAR_WAIT_PIPE_PID
}

wait_tick()
{
    IFS= read -r -t "$1" -u "$wait_fd" _ || true
}

print_log()
{
    local label=$1
    local path=$2
    local line
    [[ -r $path ]] || return 0
    while IFS= read -r line; do
        printf 'THUNAR_BRINGUP_LOG source=%s line=%q\n' "$label" "$line"
    done <"$path"
}

stop_pid()
{
    local pid=${1:-}
    [[ -n $pid ]] || return 0
    kill -TERM "$pid" 2>/dev/null || true
    local ticks=0
    while kill -0 "$pid" 2>/dev/null && [[ $ticks -lt 50 ]]; do
        wait_tick 0.1
        ticks=$((ticks + 1))
    done
    if kill -0 "$pid" 2>/dev/null; then
        kill -KILL "$pid" 2>/dev/null || true
    fi
    wait "$pid" 2>/dev/null || true
}

cleanup()
{
    stop_pid "$thunar_pid"
    stop_pid "$event_pid"
    if [[ -n $sway_socket ]]; then
        /usr/bin/swaymsg -s "$sway_socket" exit >/dev/null 2>&1 || true
    fi
    stop_pid "$sway_pid"
    stop_pid "$dbus_pid"
    if [[ -n ${wait_pid:-} ]]; then
        kill -KILL "$wait_pid" 2>/dev/null || true
        wait "$wait_pid" 2>/dev/null || true
    fi
    if [[ -n ${wait_fd:-} ]]; then
        exec {wait_fd}<&-
    fi
}

result_and_exit()
{
    local status=$1
    local dbus=$2
    local sway=$3
    local wayland=$4
    local window=$5
    local first_error=$6
    printf 'THUNAR_BRINGUP_RESULT dbus=%s sway=%s wayland=%s window=%s first_error=%s\n' \
        "$dbus" "$sway" "$wayland" "$window" "$first_error"
    exit "$status"
}

trap cleanup EXIT
wait_pipe_start

if [[ ${XDG_RUNTIME_DIR:-} != "$runtime" ||
      ${LIBSEAT_BACKEND:-} != seatd ||
      ${SEATD_SOCK:-} != "$runtime/seatd.sock" ]]; then
    result_and_exit 1 0 0 0 0 session-environment
fi

mkdir -p "$runtime" /root /root/.cache /root/.config /root/.local/share /etc
export HOME=/root
export XDG_CACHE_HOME=/root/.cache
export XDG_CONFIG_HOME=/root/.config
export XDG_DATA_HOME=/root/.local/share
export XDG_DATA_DIRS=/usr/local/share:/usr/share

machine_id_source=existing
if [[ ! -s /etc/machine-id ]]; then
    machine_id_source=generated
    if ! /usr/bin/dbus-uuidgen --ensure=/etc/machine-id; then
        result_and_exit 1 0 0 0 0 machine-id-generation
    fi
fi
machine_id=
IFS= read -r machine_id </etc/machine-id || true
if [[ ! $machine_id =~ ^[0-9a-fA-F]{32}$ ]]; then
    result_and_exit 1 0 0 0 0 machine-id-invalid
fi
printf 'THUNAR_BRINGUP_MACHINE_ID source=%s format=hex32\n' "$machine_id_source"

rm -f "$dbus_socket" "$dbus_log" "$dbus_probe_log" "$sway_log" \
    "$thunar_log" "$event_log" "$config"
export DBUS_SESSION_BUS_ADDRESS="unix:path=$dbus_socket"
/usr/bin/dbus-daemon --session \
    --address="$DBUS_SESSION_BUS_ADDRESS" \
    --nofork --nopidfile --print-address=1 >"$dbus_log" 2>&1 &
dbus_pid=$!

ticks=0
while [[ ! -S $dbus_socket && $ticks -lt $wait_limit ]]; do
    if ! kill -0 "$dbus_pid" 2>/dev/null; then
        print_log dbus "$dbus_log"
        result_and_exit 1 0 0 0 0 dbus-daemon-exit
    fi
    wait_tick 0.1
    ticks=$((ticks + 1))
done
if [[ ! -S $dbus_socket ]]; then
    print_log dbus "$dbus_log"
    result_and_exit 1 0 0 0 0 dbus-socket-timeout
fi
printf 'THUNAR_BRINGUP_DBUS_SOCKET path=%s type=unix\n' "$dbus_socket"

if ! /usr/bin/dbus-send --session --print-reply \
    --dest=org.freedesktop.DBus \
    /org/freedesktop/DBus org.freedesktop.DBus.ListNames \
    >"$dbus_probe_log" 2>&1; then
    print_log dbus-send "$dbus_probe_log"
    print_log dbus "$dbus_log"
    result_and_exit 1 0 0 0 0 dbus-list-names
fi
if ! grep -Fq 'org.freedesktop.DBus' "$dbus_probe_log"; then
    print_log dbus-send "$dbus_probe_log"
    result_and_exit 1 0 0 0 0 dbus-list-names-reply
fi
printf 'THUNAR_BRINGUP_DBUS_PASS daemon=alive list_names=1 address=%s\n' \
    "$DBUS_SESSION_BUS_ADDRESS"

while IFS= read -r line; do
    printf '%s\n' "$line"
done </cmd/phase4_gui_benchmark.conf >"$config"
printf '%s\n' \
    'for_window [app_id="thunar"] floating enable, resize set width 720 px height 540 px, focus' \
    'for_window [app_id="org.xfce.thunar"] floating enable, resize set width 720 px height 540 px, focus' \
    >>"$config"

unset WAYLAND_DISPLAY SWAYSOCK
/usr/bin/sway -c "$config" >"$sway_log" 2>&1 &
sway_pid=$!

ticks=0
while [[ $ticks -lt $wait_limit ]]; do
    shopt -s nullglob
    sockets=("$runtime"/sway-ipc.*.sock)
    shopt -u nullglob
    if [[ ${#sockets[@]} -eq 1 ]]; then
        sway_socket=${sockets[0]}
        break
    fi
    if ! kill -0 "$sway_pid" 2>/dev/null; then
        print_log sway "$sway_log"
        result_and_exit 1 1 0 0 0 sway-exit-before-socket
    fi
    wait_tick 0.1
    ticks=$((ticks + 1))
done
if [[ -z $sway_socket ]]; then
    print_log sway "$sway_log"
    result_and_exit 1 1 0 0 0 sway-socket-timeout
fi
export SWAYSOCK=$sway_socket
printf 'THUNAR_BRINGUP_SWAY_PASS socket=%s\n' "$sway_socket"

ticks=0
while [[ $ticks -lt $wait_limit ]]; do
    shopt -s nullglob
    displays=("$runtime"/wayland-*)
    shopt -u nullglob
    for path in "${displays[@]}"; do
        [[ $path == *.lock ]] && continue
        wayland_display=${path##*/}
        break 2
    done
    wait_tick 0.1
    ticks=$((ticks + 1))
done
if [[ -z $wayland_display ]]; then
    print_log sway "$sway_log"
    result_and_exit 1 1 1 0 0 wayland-display-timeout
fi
export WAYLAND_DISPLAY=$wayland_display
printf 'THUNAR_BRINGUP_WAYLAND_PASS display=%s\n' "$wayland_display"

/cmd/lpr_sway_event_monitor.elf "$sway_socket" >"$event_log" 2>&1 &
event_pid=$!
ticks=0
while ! grep -Eq '"first"[[:space:]]*:[[:space:]]*true' "$event_log" 2>/dev/null; do
    if ! kill -0 "$event_pid" 2>/dev/null; then
        print_log sway-event "$event_log"
        result_and_exit 1 1 1 1 0 sway-window-subscribe
    fi
    if [[ $ticks -ge $wait_limit ]]; then
        print_log sway-event "$event_log"
        result_and_exit 1 1 1 1 0 sway-window-subscribe-timeout
    fi
    wait_tick 0.1
    ticks=$((ticks + 1))
done
printf 'THUNAR_BRINGUP_WINDOW_MONITOR_PASS subscription=window,tick\n'

GDK_BACKEND=wayland /usr/bin/thunar >"$thunar_log" 2>&1 &
thunar_pid=$!
printf 'THUNAR_BRINGUP_APP_START pid=%s backend=wayland\n' "$thunar_pid"

ticks=0
window_event=
while [[ $ticks -lt $wait_limit ]]; do
    window_event=$(grep -m1 -E '"change"[[:space:]]*:[[:space:]]*"new"' "$event_log" 2>/dev/null || true)
    [[ -n $window_event ]] && break
    wait_tick 0.1
    ticks=$((ticks + 1))
done

if [[ -z $window_event ]]; then
    app_state=exited
    if kill -0 "$thunar_pid" 2>/dev/null; then
        app_state=alive
    fi
    printf 'THUNAR_BRINGUP_APP_NO_WINDOW pid=%s state=%s\n' "$thunar_pid" "$app_state"
    print_log thunar "$thunar_log"
    print_log sway-event "$event_log"
    result_and_exit 1 1 1 1 0 thunar-window-timeout
fi

pid_match=0
if [[ $window_event =~ \"pid\"[[:space:]]*:[[:space:]]*$thunar_pid([,}]) ]]; then
    pid_match=1
fi
app_id_match=0
if grep -Eq '\"app_id\"[[:space:]]*:[[:space:]]*\"(thunar|org\.xfce\.thunar)\"' \
    <<<"$window_event"; then
    app_id_match=1
fi
printf 'THUNAR_BRINGUP_WINDOW_EVENT pid=%s pid_match=%s app_id_match=%s json=%s\n' \
    "$thunar_pid" "$pid_match" "$app_id_match" "$window_event"
if [[ $pid_match -ne 1 || $app_id_match -ne 1 ]]; then
    result_and_exit 1 1 1 1 0 thunar-window-identity
fi
printf 'THUNAR_BRINGUP_PASS dbus=1 sway=1 wayland=1 window=1\n'
result_and_exit 0 1 1 1 1 none
