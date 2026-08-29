#!/bin/sh
set -eu

iterations=${1:-5}
target=${2:-all}
runtime=/tmp/capabilityos-gui-runtime
home_dir=/tmp/capabilityos-gui-home
config=/opt/capabilityos-bench/linux_sandboxless_sway.conf
log_dir=/tmp/capabilityos-gui-logs
trace=${LINUX_GUI_STRACE:-0}

case "$target" in
    all|sway|foot|gedit|thunar|glycin-app-png-3) ;;
    *)
        printf 'LINUX_GUI_FAIL stage=invalid-target target=%s\n' "$target" >&2
        exit 2
        ;;
esac

now_ns()
{
    date +%s%N
}

cpu_usec()
{
    awk '$1 == "usage_usec" { print $2; exit }' /sys/fs/cgroup/cpu.stat
}

fail()
{
    printf 'LINUX_GUI_FAIL stage=%s iteration=%s target=%s\n' \
        "$1" "${iteration:-0}" "$target" >&2
    if [ -f "${sway_log:-}" ]; then
        tail -80 "$sway_log" >&2
    fi
    if [ -f "${app_log:-}" ]; then
        tail -80 "$app_log" >&2
    fi
    exit 1
}

cleanup()
{
    if [ -n "${socket:-}" ] && [ -S "${socket:-}" ]; then
        SWAYSOCK=$socket swaymsg -q exit >/dev/null 2>&1 || true
    fi
    if [ "${app_pid:-0}" -gt 0 ]; then
        kill -KILL "$app_pid" 2>/dev/null || true
    fi
    if [ "${sway_pid:-0}" -gt 0 ]; then
        kill -KILL "$sway_pid" 2>/dev/null || true
        wait "$sway_pid" 2>/dev/null || true
    fi
    if [ "${dbus_pid:-0}" -gt 0 ]; then
        kill -TERM "$dbus_pid" 2>/dev/null || true
        wait "$dbus_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

wait_for_socket()
{
    ticks=0
    while [ "$ticks" -lt 1000 ]; do
        socket=$(find "$runtime" -maxdepth 1 -type s -name 'sway-ipc.*.sock' \
            -print -quit 2>/dev/null || true)
        display=$(find "$runtime" -maxdepth 1 -type s -name 'wayland-*' \
            -print -quit 2>/dev/null || true)
        if [ -n "$socket" ] && [ -n "$display" ]; then
            SWAYSOCK=$socket swaymsg -r -t get_outputs 2>/dev/null \
                | jq -e 'any(.[]; .active == true)' >/dev/null && return 0
        fi
        kill -0 "$sway_pid" 2>/dev/null || return 1
        sleep 0.01
        ticks=$((ticks + 1))
    done
    return 1
}

wait_for_fallback()
{
    ticks=0
    while [ "$ticks" -lt 200 ]; do
        grep -Fq 'WARNING: Glycin running without sandbox.' "$sway_log" \
            2>/dev/null && return 0
        kill -0 "$sway_pid" 2>/dev/null || return 1
        sleep 0.01
        ticks=$((ticks + 1))
    done
    return 1
}

wait_for_app()
{
    wanted=$1
    ticks=0
    while [ "$ticks" -lt 1000 ]; do
        SWAYSOCK=$socket swaymsg -r -t get_tree 2>/dev/null \
            | jq -e --arg wanted "$wanted" \
                '.. | objects | select((.app_id? // "") | contains($wanted))' \
                >/dev/null && return 0
        kill -0 "$app_pid" 2>/dev/null || return 1
        sleep 0.01
        ticks=$((ticks + 1))
    done
    return 1
}

measure_app()
{
    app=$1
    app_log="$log_dir/$app-$iteration.log"
    cpu_before=$(cpu_usec)
    start=$(now_ns)
    case "$app" in
        foot)
            if [ "$trace" = 1 ]; then
                strace -ff -qq -tt -T -o "$log_dir/strace-foot-$iteration" \
                    foot /bin/sh -c 'exec sleep 60' >"$app_log" 2>&1 &
            else
                foot /bin/sh -c 'exec sleep 60' >"$app_log" 2>&1 &
            fi
            wanted=foot
            ;;
        gedit)
            if [ "$trace" = 1 ]; then
                GDK_BACKEND=wayland strace -ff -qq -tt -T \
                    -o "$log_dir/strace-gedit-$iteration" \
                    gedit --new-window >"$app_log" 2>&1 &
            else
                GDK_BACKEND=wayland gedit --new-window >"$app_log" 2>&1 &
            fi
            wanted=gedit
            ;;
        thunar)
            if [ "$trace" = 1 ]; then
                GDK_BACKEND=wayland strace -ff -qq -tt -T \
                    -o "$log_dir/strace-thunar-$iteration" \
                    thunar >"$app_log" 2>&1 &
            else
                GDK_BACKEND=wayland thunar >"$app_log" 2>&1 &
            fi
            wanted=thunar
            ;;
    esac
    app_pid=$!
    wait_for_app "$wanted" || fail "$app-window"
    finish=$(now_ns)
    cpu_after=$(cpu_usec)
    printf 'LINUX_GUI_RESULT iteration=%s phase=%s wall_ms=%s cpu_ms=%s pid=%s\n' \
        "$iteration" "$app" \
        "$((finish - start))" "$((cpu_after - cpu_before))" "$app_pid" \
        | awk -F'[ =]' '{ printf "%s iteration=%s phase=%s wall_ms=%.3f cpu_ms=%.3f pid=%s\n", $1, $3, $5, $7 / 1000000, $9 / 1000, $11 }'

    SWAYSOCK=$socket swaymsg -q "[app_id=\"$wanted\"] kill" \
        >/dev/null 2>&1 || true
    kill -KILL "$app_pid" 2>/dev/null || true
    wait "$app_pid" 2>/dev/null || true
    app_pid=0
}

rm -rf "$log_dir"
mkdir -p "$log_dir"
printf 'LINUX_GUI_ENV alpine=%s sway=%s foot=%s gedit=%s gtk=%s gdk_pixbuf=%s\n' \
    "$(cat /etc/alpine-release)" \
    "$(apk list --installed sway 2>/dev/null | awk -F' ' 'NR == 1 { print $1 }')" \
    "$(apk list --installed foot 2>/dev/null | awk -F' ' 'NR == 1 { print $1 }')" \
    "$(apk list --installed gedit 2>/dev/null | awk -F' ' 'NR == 1 { print $1 }')" \
    "$(apk list --installed 'gtk+3.0' 2>/dev/null | awk -F' ' 'NR == 1 { print $1 }')" \
    "$(apk list --installed gdk-pixbuf 2>/dev/null | awk -F' ' 'NR == 1 { print $1 }')"

if [ "$target" = glycin-app-png-3 ]; then
    iteration=1
    while [ "$iteration" -le "$iterations" ]; do
        app_log="$log_dir/glycin-$iteration.log"
        cpu_before=$(cpu_usec)
        start=$(now_ns)
        /usr/local/bin/glycin-app-probe \
            /usr/share/backgrounds/sway/Sway_Wallpaper_Blue_1920x1080.png 3 \
            >"$app_log" 2>&1 || fail glycin-probe
        finish=$(now_ns)
        cpu_after=$(cpu_usec)
        grep -Fq 'WARNING: Glycin running without sandbox.' "$app_log" \
            || fail glycin-fallback
        cat "$app_log"
        printf 'LINUX_GUI_RESULT iteration=%s phase=glycin-app-png-3 wall_ms=%s cpu_ms=%s pid=0\n' \
            "$iteration" "$((finish - start))" "$((cpu_after - cpu_before))" \
            | awk -F'[ =]' '{ printf "%s iteration=%s phase=%s wall_ms=%.3f cpu_ms=%.3f pid=%s\n", $1, $3, $5, $7 / 1000000, $9 / 1000, $11 }'
        iteration=$((iteration + 1))
    done
    trap - EXIT INT TERM
    exit 0
fi

iteration=1
while [ "$iteration" -le "$iterations" ]; do
    cleanup
    socket=
    display=
    sway_pid=0
    app_pid=0
    dbus_pid=0
    rm -rf "$runtime" "$home_dir"
    mkdir -p "$runtime" "$home_dir"
    chmod 700 "$runtime"
    sway_log="$log_dir/sway-$iteration.log"

    export XDG_RUNTIME_DIR=$runtime
    export HOME=$home_dir
    export WLR_BACKENDS=headless
    export WLR_HEADLESS_OUTPUTS=1
    export WLR_RENDERER=pixman
    export WLR_LIBINPUT_NO_DEVICES=1
    unset WAYLAND_DISPLAY SWAYSOCK DISPLAY

    dbus_socket="$runtime/session-bus"
    export DBUS_SESSION_BUS_ADDRESS="unix:path=$dbus_socket"
    dbus-daemon --session --address="$DBUS_SESSION_BUS_ADDRESS" \
        --nofork --nopidfile >"$log_dir/dbus-$iteration.log" 2>&1 &
    dbus_pid=$!
    ticks=0
    while [ "$ticks" -lt 500 ] && [ ! -S "$dbus_socket" ]; do
        kill -0 "$dbus_pid" 2>/dev/null || fail dbus-start
        sleep 0.01
        ticks=$((ticks + 1))
    done
    [ -S "$dbus_socket" ] || fail dbus-timeout

    cpu_before=$(cpu_usec)
    start=$(now_ns)
    if [ "$trace" = 1 ]; then
        strace -ff -qq -tt -T -o "$log_dir/strace-sway-$iteration" \
            sway -c "$config" >"$sway_log" 2>&1 &
    else
        sway -c "$config" >"$sway_log" 2>&1 &
    fi
    sway_pid=$!
    wait_for_socket || fail sway-ready
    wait_for_fallback || fail glycin-fallback
    finish=$(now_ns)
    cpu_after=$(cpu_usec)
    display=${display##*/}
    export SWAYSOCK=$socket WAYLAND_DISPLAY=$display
    printf 'LINUX_GUI_RESULT iteration=%s phase=sway wall_ms=%s cpu_ms=%s pid=%s\n' \
        "$iteration" "$((finish - start))" "$((cpu_after - cpu_before))" "$sway_pid" \
        | awk -F'[ =]' '{ printf "%s iteration=%s phase=%s wall_ms=%.3f cpu_ms=%.3f pid=%s\n", $1, $3, $5, $7 / 1000000, $9 / 1000, $11 }'

    case "$target" in
        all)
            measure_app foot
            measure_app gedit
            ;;
        foot|gedit|thunar)
            measure_app "$target"
            ;;
    esac
    iteration=$((iteration + 1))
done

cleanup
trap - EXIT INT TERM
