#!/bin/bash
set -u

runtime=/run/user/0
config=/cmd/phase4_gui_benchmark.conf
target=${1:?missing-target}
wait_limit=600
event_log=/tmp/gui-startup-window-events.log
app_log=/tmp/gui-startup-app.log

fail()
{
    printf 'GUI_STARTUP_FAIL stage=%s target=%s\n' "$1" "$target"
    exit 1
}

wait_pipe_start()
{
    coproc GUI_WAIT_PIPE { while IFS= read -r _; do :; done; }
    wait_fd=${GUI_WAIT_PIPE[0]}
    wait_pid=$GUI_WAIT_PIPE_PID
}

wait_tick()
{
    IFS= read -r -t "$1" -u "$wait_fd" _ || true
}

cleanup()
{
    if [[ ${monitor_pid:-0} -gt 0 ]]; then
        kill -KILL "$monitor_pid" 2>/dev/null || true
    fi
    if [[ ${LPR_PROFILE_GRACEFUL:-0} == 1 && ${app_pid:-0} -gt 0 && -n ${socket:-} ]]; then
        /usr/bin/swaymsg -s "$socket" \
            "[app_id=\"$target\"] kill" >/dev/null 2>&1 || true
        for ((stop_tick = 0; stop_tick < 40; stop_tick++)); do
            [[ -r /proc/$app_pid/stat ]] || break
            [[ $(awk '{ print $3 }' "/proc/$app_pid/stat" 2>/dev/null) == Z ]] && break
            wait_tick 0.05
        done
        kill -TERM "$app_pid" 2>/dev/null || true
        wait_tick 0.05
        kill -KILL "$app_pid" 2>/dev/null || true
        wait "$app_pid" 2>/dev/null || true
        app_pid=0
    fi
    if [[ ${app_pid:-0} -gt 0 ]]; then
        kill -KILL "$app_pid" 2>/dev/null || true
    fi
    if [[ ${LPR_PROFILE_GRACEFUL:-0} == 1 && ${sway_pid:-0} -gt 0 && -n ${socket:-} ]]; then
        /usr/bin/swaymsg -s "$socket" exit >/dev/null 2>&1 || true
        for ((stop_tick = 0; stop_tick < 40; stop_tick++)); do
            [[ -r /proc/$sway_pid/stat ]] || break
            [[ $(awk '{ print $3 }' "/proc/$sway_pid/stat" 2>/dev/null) == Z ]] && break
            wait_tick 0.05
        done
        kill -TERM "$sway_pid" 2>/dev/null || true
        wait_tick 0.05
        kill -KILL "$sway_pid" 2>/dev/null || true
        wait "$sway_pid" 2>/dev/null || true
        sway_pid=0
    fi
    if [[ ${sway_pid:-0} -gt 0 ]]; then
        kill -KILL "$sway_pid" 2>/dev/null || true
    fi
    if [[ ${wait_pid:-0} -gt 0 ]]; then
        kill -KILL "$wait_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT
wait_pipe_start

if ! apk info -e gedit >/dev/null 2>&1; then
    fail gedit-not-installed
fi
gedit_version=$(apk info -v gedit 2>/dev/null | head -n 1)
[[ -n $gedit_version ]] || gedit_version=gedit
[[ -x /usr/bin/gedit ]] || fail gedit-executable
printf 'GUI_STARTUP_PRECHECK package=%s installed=1 target=%s\n' \
    "$gedit_version" "$target"

case "$target" in
glycin-app-png|glycin-app-png-3|glycin-app-png-small-3)
    iterations=1
    image=/usr/share/backgrounds/sway/Sway_Wallpaper_Blue_1920x1080.png
    [[ $target == *-3 ]] && iterations=3
    if [[ $target == glycin-app-png-small-3 ]]; then
        image=/usr/share/icons/Adwaita/16x16/mimetypes/text-x-preview.png
    fi
    printf 'GUI_STARTUP_EXEC app=%s input=%s iterations=%s\n' \
        "$target" "$image" "$iterations"
    /cmd/lpr_glycin_app_probe.elf \
        "$image" "$iterations" >"$app_log" 2>&1 || {
        /bin/cat "$app_log"
        fail glycin-app-probe-exec
    }
    /bin/cat "$app_log"
    printf 'GUI_STARTUP_READY app=%s iterations=%s\n' "$target" "$iterations"
    exit 0
    ;;
glycin-png)
    /bin/rm -f /tmp/gui-startup-wallpaper.png
    printf 'GUI_STARTUP_EXEC app=glycin-png input=sway-wallpaper\n'
    /usr/bin/gdk-pixbuf-thumbnailer \
        /usr/share/backgrounds/sway/Sway_Wallpaper_Blue_1920x1080.png \
        /tmp/gui-startup-wallpaper.png >"$app_log" 2>&1 || {
        /bin/cat "$app_log"
        fail glycin-png-exec
    }
    [[ -s /tmp/gui-startup-wallpaper.png ]] || fail glycin-png-output
    printf 'GUI_STARTUP_READY app=glycin-png output_bytes=%s\n' \
        "$(stat -c %s /tmp/gui-startup-wallpaper.png)"
    exit 0
    ;;
glycin-svg)
    /bin/rm -f /tmp/gui-startup-image-missing.png
    printf 'GUI_STARTUP_EXEC app=glycin-svg input=adwaita-image-missing\n'
    /usr/bin/gdk-pixbuf-thumbnailer \
        /usr/share/icons/Adwaita/scalable/status/image-missing.svg \
        /tmp/gui-startup-image-missing.png >"$app_log" 2>&1 || {
        /bin/cat "$app_log"
        fail glycin-svg-exec
    }
    [[ -s /tmp/gui-startup-image-missing.png ]] || fail glycin-svg-output
    printf 'GUI_STARTUP_READY app=glycin-svg output_bytes=%s\n' \
        "$(stat -c %s /tmp/gui-startup-image-missing.png)"
    exit 0
    ;;
sway|foot|gedit) ;;
*) fail invalid-target ;;
esac

unset WAYLAND_DISPLAY SWAYSOCK
printf 'GUI_STARTUP_EXEC app=sway target=%s\n' "$target"
/usr/bin/sway -c "$config" > /tmp/gui-startup-sway.log 2>&1 &
sway_pid=$!

socket=
display=
ticks=0
while [[ $ticks -lt $wait_limit ]]; do
    shopt -s nullglob
    sockets=("$runtime"/sway-ipc.*.sock)
    displays=("$runtime"/wayland-*)
    shopt -u nullglob
    if [[ ${#sockets[@]} -eq 1 ]]; then
        socket=${sockets[0]}
    fi
    for path in "${displays[@]}"; do
        [[ $path == *.lock ]] && continue
        display=${path##*/}
        break
    done
    [[ -n $socket && -n $display ]] && break
    kill -0 "$sway_pid" 2>/dev/null || {
        /bin/cat /tmp/gui-startup-sway.log
        fail sway-exited
    }
    wait_tick 0.05
    ticks=$((ticks + 1))
done
[[ -n $socket ]] || fail sway-socket-timeout
[[ -n $display ]] || fail wayland-display-timeout
export SWAYSOCK=$socket WAYLAND_DISPLAY=$display

P4_BACKGROUND_PROBE=1 /cmd/lpr_wayland_animation_bench.elf \
    >/tmp/gui-startup-background.log 2>&1 || {
    /bin/cat /tmp/gui-startup-background.log
    fail sway-presentation
}

/bin/rm -f "$event_log"
/cmd/lpr_sway_event_monitor.elf "$socket" >"$event_log" 2>&1 &
monitor_pid=$!
ticks=0
while ! /bin/grep -Eq '"first"[[:space:]]*:[[:space:]]*true' \
    "$event_log" 2>/dev/null; do
    kill -0 "$monitor_pid" 2>/dev/null || fail sway-event-monitor
    [[ $ticks -lt $wait_limit ]] || fail sway-event-timeout
    wait_tick 0.05
    ticks=$((ticks + 1))
done
printf 'GUI_STARTUP_READY app=sway target=%s display=%s\n' "$target" "$display"
[[ $target != sway ]] || exit 0

printf 'GUI_STARTUP_EXEC app=%s\n' "$target"
printf 'GUI_STARTUP_APP_CONTEXT app=%s display=%s socket=%s socket_exists=%s\n' \
    "$target" "$WAYLAND_DISPLAY" "$SWAYSOCK" \
    "$([[ -S $runtime/$WAYLAND_DISPLAY ]] && echo 1 || echo 0)"
if [[ $target == foot ]]; then
    /usr/bin/foot /bin/sh -c 'exec /bin/sleep 60' >"$app_log" 2>&1 &
else
    GDK_BACKEND=wayland /usr/bin/gedit --new-window >"$app_log" 2>&1 &
fi
app_pid=$!
pid_token="\"pid\": $app_pid"

ticks=0
window_event=
while [[ $ticks -lt $wait_limit ]]; do
    while IFS= read -r line; do
        [[ $line == *'"change": "new"'* ]] || continue
        if [[ $line == *"$pid_token"* ]]; then
            window_event=$line
            break 2
        fi
        if [[ $target == foot && $line == *'"app_id": "foot"'* ]]; then
            window_event=$line
            break 2
        fi
        if [[ $target == gedit && $line == *'"app_id": "gedit"'* ]]; then
            window_event=$line
            break 2
        fi
    done <"$event_log"
    wait_tick 0.05
    ticks=$((ticks + 1))
done
if [[ -z $window_event ]]; then
    /bin/cat "$app_log"
    /bin/cat "$event_log"
    fail app-window-timeout
fi
printf 'GUI_STARTUP_READY app=%s pid=%s\n' "$target" "$app_pid"
