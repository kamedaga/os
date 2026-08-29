#!/bin/bash

set -u

runtime=/run/user/0
console=/dev/hvc0
config="$runtime/key-launch-phase.conf"
dbus_socket="$runtime/key-launch-dbus"
dbus_log="$runtime/key-launch-dbus.log"
sway_log="$runtime/key-launch-sway.log"
event_log="$runtime/key-launch-events.log"
event_ready="$runtime/key-launch-event-ready"
wait_limit=600

dbus_pid=
sway_pid=
event_relay_pid=
sway_socket=

now_ns()
{
    printf '%s000' "${EPOCHREALTIME/./}"
}

emit()
{
    printf '%s\n' "$*" >"$console"
}

wait_tick()
{
    IFS= read -r -t "$1" _ || true
}

pid_is_running()
{
    local pid=${1:-0}
    local state=
    kill -0 "$pid" 2>/dev/null || return 1
    if [[ -r /proc/$pid/stat ]]; then
        read -r _ _ state _ <"/proc/$pid/stat" 2>/dev/null || true
        [[ $state != Z ]] || return 1
    fi
    return 0
}

stop_pid()
{
    local pid=${1:-0}
    pid_is_running "$pid" || return 0
    kill -TERM "$pid" 2>/dev/null || true
    for ((tick = 0; tick < 30; tick++)); do
        pid_is_running "$pid" || break
        wait_tick 0.05
    done
    pid_is_running "$pid" && kill -KILL "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

cleanup()
{
    for app in foot thunar; do
        pid=
        if [[ -r $runtime/key-phase-$app.pid ]]; then
            IFS= read -r pid <"$runtime/key-phase-$app.pid" || true
        fi
        stop_pid "${pid:-0}"
    done
    if [[ -n $sway_socket ]]; then
        /usr/bin/swaymsg -s "$sway_socket" exit >/dev/null 2>&1 || true
    fi
    stop_pid "${sway_pid:-0}"
    stop_pid "${event_relay_pid:-0}"
    stop_pid "${dbus_pid:-0}"
}
trap cleanup EXIT

mkdir -p "$runtime" /root /root/.cache /root/.config /root/.local/share
export HOME=/root
export XDG_RUNTIME_DIR=$runtime
export XDG_CACHE_HOME=/root/.cache
export XDG_CONFIG_HOME=/root/.config
export XDG_DATA_HOME=/root/.local/share
export XDG_DATA_DIRS=/usr/local/share:/usr/share

report_image_stack()
{
    local stage=$1
    local gedit=0
    local libglycin=0
    local glycin_image=0
    local glycin_svg=0
    local loader_entries=0
    apk info -e gedit >/dev/null 2>&1 && gedit=1
    apk info -e libglycin >/dev/null 2>&1 && libglycin=1
    apk info -e glycin-image-rs >/dev/null 2>&1 && glycin_image=1
    apk info -e glycin-svg >/dev/null 2>&1 && glycin_svg=1
    if [[ -r /usr/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache ]]; then
        loader_entries=$(/bin/grep -ci glycin \
            /usr/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache 2>/dev/null || true)
    fi
    emit "KEY_PHASE_IMAGE_STACK stage=$stage gedit=$gedit libglycin=$libglycin glycin_image=$glycin_image glycin_svg=$glycin_svg glycin_loader_entries=$loader_entries"
}

/bin/rm -f "$dbus_socket" "$dbus_log" "$sway_log" "$event_log" \
    "$event_ready" "$config" "$runtime"/key-phase-*.pid \
    "$runtime"/key-phase-*.done "$runtime"/key-phase-*.log

report_image_stack before
if [[ ${KEY_PHASE_APK_ADD_GEDIT:-0} == 1 ]] && \
    ! apk info -e gedit >/dev/null 2>&1
then
    emit "KEY_PHASE_APK stage=update_begin guest_ns=$(now_ns)"
    if ! apk update >"$runtime/key-phase-apk.log" 2>&1; then
        /bin/cat "$runtime/key-phase-apk.log" >"$console"
        emit 'KEY_PHASE_FAIL stage=apk_update'
        exit 1
    fi
    emit "KEY_PHASE_APK stage=add_begin guest_ns=$(now_ns)"
    if ! apk add gedit >>"$runtime/key-phase-apk.log" 2>&1; then
        if apk info -e gedit >/dev/null 2>&1 && [[ -x /usr/bin/gedit ]]; then
            emit "KEY_PHASE_APK stage=add_partial guest_ns=$(now_ns)"
        else
            /bin/cat "$runtime/key-phase-apk.log" >"$console"
            emit 'KEY_PHASE_FAIL stage=apk_add_gedit'
            exit 1
        fi
    fi
    emit "KEY_PHASE_APK stage=add_done guest_ns=$(now_ns)"
fi
report_image_stack after

if [[ ${KEY_PHASE_NO_AT_BRIDGE:-0} == 1 ]]; then
    export NO_AT_BRIDGE=1
    emit "KEY_PHASE_ENV no_at_bridge=1 guest_ns=$(now_ns)"
fi

if [[ ! -s /etc/machine-id ]]; then
    /usr/bin/dbus-uuidgen --ensure=/etc/machine-id || {
        emit 'KEY_PHASE_FAIL stage=machine_id'
        exit 1
    }
fi
export DBUS_SESSION_BUS_ADDRESS="unix:path=$dbus_socket"
/usr/bin/dbus-daemon --session --address="$DBUS_SESSION_BUS_ADDRESS" \
    --nofork --nopidfile --print-address=1 >"$dbus_log" 2>&1 &
dbus_pid=$!
for ((tick = 0; tick < wait_limit; tick++)); do
    [[ -S $dbus_socket ]] && break
    pid_is_running "$dbus_pid" || {
        emit 'KEY_PHASE_FAIL stage=dbus_exit'
        exit 1
    }
    wait_tick 0.05
done
[[ -S $dbus_socket ]] || {
    emit 'KEY_PHASE_FAIL stage=dbus_timeout'
    exit 1
}

while IFS= read -r line; do
    printf '%s\n' "$line"
done </cmd/phase4_gui_benchmark.conf >"$config"
printf '%s\n' \
    'bindsym F1 exec /cmd/lpr_key_launch_probe.elf launch foot' \
    'bindsym F2 exec /cmd/lpr_key_launch_probe.elf launch thunar' \
    'bindsym F3 exec /cmd/lpr_key_launch_probe.elf close foot' \
    'bindsym F4 exec /cmd/lpr_key_launch_probe.elf close thunar' \
    'for_window [app_id="thunar"] floating enable, resize set width 720 px height 540 px, focus' \
    'for_window [app_id="org.xfce.thunar"] floating enable, resize set width 720 px height 540 px, focus' \
    >>"$config"

unset WAYLAND_DISPLAY SWAYSOCK
/usr/bin/sway -c "$config" >"$sway_log" 2>&1 &
sway_pid=$!
for ((tick = 0; tick < wait_limit; tick++)); do
    shopt -s nullglob
    sockets=("$runtime"/sway-ipc.*.sock)
    shopt -u nullglob
    if [[ ${#sockets[@]} -eq 1 ]]; then
        sway_socket=${sockets[0]}
        break
    fi
    pid_is_running "$sway_pid" || {
        emit 'KEY_PHASE_FAIL stage=sway_exit'
        exit 1
    }
    wait_tick 0.05
done
[[ -n $sway_socket ]] || {
    emit 'KEY_PHASE_FAIL stage=sway_socket_timeout'
    exit 1
}
export SWAYSOCK=$sway_socket

(
    /cmd/lpr_sway_event_monitor.elf "$sway_socket" |
    while IFS= read -r line; do
        guest_ns=$(now_ns)
        printf '%s\n' "$line" >>"$event_log"
        emit "KEY_PHASE_SWAY_EVENT guest_ns=$guest_ns json=$line"
        if [[ $line == *'"first": true'* || $line == *'"first":true'* ]]; then
            : >"$event_ready"
        fi
    done
) &
event_relay_pid=$!
for ((tick = 0; tick < wait_limit; tick++)); do
    [[ -e $event_ready ]] && break
    pid_is_running "$event_relay_pid" || {
        emit 'KEY_PHASE_FAIL stage=event_monitor_exit'
        exit 1
    }
    wait_tick 0.05
done
[[ -e $event_ready ]] || {
    emit 'KEY_PHASE_FAIL stage=event_monitor_timeout'
    exit 1
}

/cmd/lpr_tsc_calibrate.elf

for app in ${KEY_PHASE_APPS:-foot thunar}; do
    key=f1
    [[ $app == thunar ]] && key=f2
    emit "KEY_PHASE_READY app=$app key=$key guest_ns=$(now_ns)"
    done_file="$runtime/key-phase-$app.done"
    for ((tick = 0; tick < wait_limit; tick++)); do
        [[ -e $done_file ]] && break
        pid_is_running "$sway_pid" || {
            emit "KEY_PHASE_FAIL stage=sway_exit_during_$app"
            exit 1
        }
        wait_tick 0.05
    done
    [[ -e $done_file ]] || {
        emit "KEY_PHASE_FAIL stage=${app}_exit_timeout"
        exit 1
    }
    if [[ ${KEY_PHASE_DUMP_APP_LOGS:-0} == 1 ]]; then
        app_log="$runtime/key-phase-$app.log"
        if [[ -s $app_log ]]; then
            while IFS= read -r line; do
                emit "KEY_PHASE_APP_LOG app=$app line=$line"
            done <"$app_log"
        fi
    fi
    emit "KEY_PHASE_APP_DONE app=$app guest_ns=$(now_ns)"
    # The host starts the next app from a stable framebuffer baseline.  Give
    # Sway one repaint after the just-killed client has been removed; otherwise
    # the next baseline can still contain the previous window even though the
    # child has already been reaped.
    [[ $app != foot ]] || wait_tick 0.25
done

emit "KEY_PHASE_COMPLETE guest_ns=$(now_ns)"
