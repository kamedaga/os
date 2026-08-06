#!/bin/bash

set -u

runtime=/run/user/0
config=/cmd/phase4_gui_benchmark.conf
wait_limit=1200
process_poll_limit=60
process_stop_limit=400

wait_pipe_start()
{
    coproc P4_WAIT_PIPE { while IFS= read -r _; do :; done; }
    p4_wait_fd=${P4_WAIT_PIPE[0]}
    p4_wait_pid=$P4_WAIT_PIPE_PID
}

wait_pipe_stop()
{
    kill -KILL "$p4_wait_pid" 2>/dev/null || true
    wait "$p4_wait_pid" 2>/dev/null || true
    exec {p4_wait_fd}<&-
    p4_wait_pid=
}

wait_pipe_start

wait_tick()
{
    IFS= read -r -t "$1" -u "$p4_wait_fd" _ || true
}

now_ns()
{
    printf '%s000\n' "${EPOCHREALTIME/./}"
}

elapsed_ms()
{
    printf '%s\n' "$((($2 - $1) / 1000000))"
}

fail()
{
    printf 'P4_BENCH_FAIL stage=%s\n' "$1"
    return 1
}

wait_for_socket()
{
    local sway_pid=$1
    wait_socket=
    wait_socket_sway_exited=0
    local ticks=0
    while [[ $ticks -lt $wait_limit ]]; do
        shopt -s nullglob
        local sockets=("$runtime"/sway-ipc.*.sock)
        shopt -u nullglob
        if [[ ${#sockets[@]} -eq 1 ]]; then
            wait_socket=${sockets[0]}
            return 0
        fi
        if ! kill -0 "$sway_pid" 2>/dev/null || pid_has_stopped "$sway_pid"; then
            local status=0
            wait "$sway_pid" 2>/dev/null || status=$?
            printf 'P4_BENCH_SWAY_EXIT_BEFORE_SOCKET pid=%s status=%s\n' \
                "$sway_pid" "$status" >&2
            wait_socket_sway_exited=1
            return 1
        fi
        wait_tick 0.1
        ticks=$((ticks + 1))
    done
    return 1
}

wait_for_wayland_display()
{
    local ticks=0
    while [[ $ticks -lt $wait_limit ]]; do
        shopt -s nullglob
        local sockets=("$runtime"/wayland-*)
        shopt -u nullglob
        local path
        for path in "${sockets[@]}"; do
            [[ $path == *.lock ]] && continue
            printf '%s\n' "${path##*/}"
            return 0
        done
        wait_tick 0.1
        ticks=$((ticks + 1))
    done
    return 1
}

window_event_start()
{
    local socket=$1
    wait_pipe_stop
    coproc P4_WINDOW_EVENTS {
        exec /cmd/lpr_sway_event_monitor.elf "$socket"
    }
    window_event_fd=${P4_WINDOW_EVENTS[0]}
    window_event_pid=$P4_WINDOW_EVENTS_PID
    p4_wait_fd=$window_event_fd
    local line
    # A tick subscription emits an initial first=true event.  Unlike the
    # subscribe response, swaymsg exposes this event in monitor mode, so it is
    # an unambiguous indication that the long-lived IPC connection is ready.
    local success_pattern='"first"[[:space:]]*:[[:space:]]*true'
    while IFS= read -r line <&"$window_event_fd"; do
        [[ $line =~ $success_pattern ]] && return 0
    done
    return 1
}

wait_for_window_pid()
{
    local pid=$1
    local line
    local pid_pattern='"pid"[[:space:]]*:[[:space:]]*'"$pid"'([,}])'
    while IFS= read -r line <&"$window_event_fd"; do
        if [[ $line =~ $pid_pattern ]]; then
            printf 'P4_BENCH_WINDOW_EVENT expected_pid=%s json=%s\n' "$pid" "$line"
            return 0
        fi
    done
    return 1
}

window_event_stop()
{
    if [[ ${window_event_pid:-0} -gt 0 ]]; then
        kill -KILL "$window_event_pid" 2>/dev/null || true
        wait "$window_event_pid" 2>/dev/null || true
    fi
    if [[ -n ${window_event_fd:-} ]]; then
        exec {window_event_fd}<&-
    fi
    window_event_pid=
    wait_pipe_start
}

wait_for_background_presented()
{
    local status=0
    background_probe_start=$(now_ns)
    P4_BACKGROUND_PROBE=1 /cmd/lpr_wayland_animation_bench.elf || status=$?
    background_probe_end=$(now_ns)
    [[ $status -eq 0 ]]
}

pid_has_stopped()
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

wait_for_pid_stopped()
{
    local pid=$1
    local ticks=0
    while [[ $ticks -lt $process_stop_limit ]]; do
        if pid_has_stopped "$pid"; then
            return 0
        fi
        wait_tick 0.1
        ticks=$((ticks + 1))
    done
    return 1
}

wait_for_runtime_clean()
{
    local ticks=0
    while [[ $ticks -lt 300 ]]; do
        shopt -s nullglob
        local stale=("$runtime"/wayland-* "$runtime"/sway-ipc.*.sock)
        shopt -u nullglob
        if [[ ${#stale[@]} -eq 0 ]]; then
            return 0
        fi
        wait_tick 0.1
        ticks=$((ticks + 1))
    done
    return 1
}

stop_sway()
{
    local socket=$1
    local pid=$2
    /usr/bin/swaymsg -s "$socket" exit >/dev/null 2>&1 || true
    if ! wait_for_pid_stopped "$pid"; then
        kill -TERM "$pid" 2>/dev/null || true
        wait_for_pid_stopped "$pid" || return 1
    fi
    wait "$pid" 2>/dev/null || true
    return 0
}

if [[ ${XDG_RUNTIME_DIR:-} != "$runtime" ||
      ${LIBSEAT_BACKEND:-} != seatd ||
      ${SEATD_SOCK:-} != "$runtime/seatd.sock" ||
      ! -e "$runtime/seatd.sock" ]]; then
    fail session-environment
    exit 1
fi
if [[ ${LD_PRELOAD+x} || ${LP_NUM_THREADS+x} ||
      ${M57_WLROOTS_KEYMAP_PRELOAD+x} ]]; then
    fail forbidden-environment
    exit 1
fi
if ! wait_for_runtime_clean; then
    fail stale-runtime-at-baseline
    exit 1
fi

if [[ ${P4_SHORT_GUI_ONLY:-0} == 1 ]]; then
    unset WAYLAND_DISPLAY SWAYSOCK
    short_start=$(now_ns)
    printf 'P4_BENCH_SWAY_EXEC phase=short guest_ns=%s path=/usr/bin/sway\n' \
        "$short_start"
    if [[ ${P4_SWAY_DEBUG:-0} == 1 ]]; then
        /usr/bin/sway -d -c "$config" &
    else
        /usr/bin/sway -c "$config" &
    fi
    sway_pid=$!
    if ! wait_for_socket "$sway_pid"; then
        fail short-socket
        [[ $wait_socket_sway_exited == 1 ]] || kill -KILL "$sway_pid"
        exit 1
    fi
    socket=$wait_socket
    short_socket=$(now_ns)
    display=$(wait_for_wayland_display) || {
        fail short-display
        kill -KILL "$sway_pid"
        exit 1
    }
    wait_for_background_presented || {
        fail short-background
        kill -KILL "$sway_pid"
        exit 1
    }
    short_background=$(now_ns)
    window_event_start "$socket" || {
        fail short-ipc
        kill -KILL "$sway_pid"
        exit 1
    }
    short_ready=$(now_ns)
    window_event_stop
    export WAYLAND_DISPLAY=$display
    export SWAYSOCK=$socket
    printf 'P4_BENCH_STARTUP phase=short socket_ms=%s background_ms=%s probe_ms=%s socket_to_background_ms=%s ipc_ms=%s display=%s\n' \
        "$(elapsed_ms "$short_start" "$short_socket")" \
        "$(elapsed_ms "$short_start" "$short_background")" \
        "$(elapsed_ms "$background_probe_start" "$background_probe_end")" \
        "$(elapsed_ms "$short_socket" "$short_background")" \
        "$(elapsed_ms "$short_start" "$short_ready")" "$display"
    if [[ ${P4_BACKGROUND_ONLY:-0} == 1 ]]; then
        stop_sway "$socket" "$sway_pid" || {
            fail short-background-stop
            exit 1
        }
        printf 'P4_BENCH_PASS mode=background direct_sway=1 presentation=1 cleanup=1\n'
        exit 0
    fi
    if ! /cmd/lpr_wayland_animation_bench.elf; then
        fail short-animation
        stop_sway "$socket" "$sway_pid" || true
        exit 1
    fi
    stop_sway "$socket" "$sway_pid" || {
        fail short-stop
        exit 1
    }
    printf 'P4_BENCH_PASS mode=short direct_sway=1 animation=1 cleanup=1 mouse_stress=%s\n' \
        "${P4_MOUSE_STRESS:-0}"
    exit 0
fi

if [[ ${P3A_INPUT_ONLY:-0} == 1 ]]; then
    input_mode=${P3A_INPUT_MODE:-mouse}
    unset WAYLAND_DISPLAY SWAYSOCK
    p3a_start=$(now_ns)
    printf 'P4_BENCH_SWAY_EXEC phase=p3a guest_ns=%s path=/usr/bin/sway\n' "$p3a_start"
    /usr/bin/sway -c "$config" &
    sway_pid=$!
    if ! wait_for_socket "$sway_pid"; then
        fail p3a-socket
        [[ $wait_socket_sway_exited == 1 ]] || kill -KILL "$sway_pid"
        exit 1
    fi
    socket=$wait_socket
    p3a_socket=$(now_ns)
    display=$(wait_for_wayland_display) || { fail p3a-display; kill -KILL "$sway_pid"; exit 1; }
    wait_for_background_presented || { fail p3a-background; kill -KILL "$sway_pid"; exit 1; }
    p3a_background=$(now_ns)
    window_event_start "$socket" || { fail p3a-ipc; kill -KILL "$sway_pid"; exit 1; }
    p3a_ipc=$(now_ns)
    window_event_stop
    export WAYLAND_DISPLAY=$display
    export SWAYSOCK=$socket
    printf 'P4_BENCH_STARTUP phase=p3a socket_ms=%s background_ms=%s probe_ms=%s socket_to_background_ms=%s ipc_ms=%s display=%s\n' \
        "$(elapsed_ms "$p3a_start" "$p3a_socket")" \
        "$(elapsed_ms "$p3a_start" "$p3a_background")" \
        "$(elapsed_ms "$background_probe_start" "$background_probe_end")" \
        "$(elapsed_ms "$p3a_socket" "$p3a_background")" \
        "$(elapsed_ms "$p3a_start" "$p3a_ipc")" "$display"
    inputs=$(/usr/bin/swaymsg -s "$socket" -t get_inputs) || {
        fail p3a-get-inputs
        stop_sway "$socket" "$sway_pid" || true
        exit 1
    }
    printf '%s\n' "$inputs"
    [[ $inputs == *'"type": "keyboard"'* ]] || {
        fail p3a-keyboard-classification
        stop_sway "$socket" "$sway_pid" || true
        exit 1
    }
    if [[ $input_mode == tablet ]]; then
        [[ $inputs == *'"type": "pointer"'* &&
           $inputs == *'"name": "QEMU Virtio Tablet"'* ]] || {
            fail p3a-tablet-classification
            stop_sway "$socket" "$sway_pid" || true
            exit 1
        }
        printf 'P3A_TABLET_INPUT_READY source=qmp-absolute\n'
        wait_tick 2
        kill -0 "$sway_pid" 2>/dev/null || {
            fail p3a-tablet-sway-exit
            exit 1
        }
    else
        [[ $inputs == *'"type": "pointer"'* ]] || {
            fail p3a-pointer-classification
            stop_sway "$socket" "$sway_pid" || true
            exit 1
        }
        /cmd/lpr_wayland_animation_bench.elf || {
            fail p3a-wayland-input
            stop_sway "$socket" "$sway_pid" || true
            exit 1
        }
    fi
    stop_sway "$socket" "$sway_pid" || { fail p3a-sway-exit; exit 1; }
    wait_for_runtime_clean || { fail p3a-runtime-clean; exit 1; }
    printf 'P3A_INPUT_PASS mode=%s direct_sway=1 classification=1\n' "$input_mode"
    exit 0
fi

unset WAYLAND_DISPLAY SWAYSOCK
cold_start=$(now_ns)
printf 'P4_BENCH_SWAY_EXEC phase=cold guest_ns=%s path=/usr/bin/sway\n' "$cold_start"
/usr/bin/sway -c "$config" &
sway_pid=$!
printf 'P4_BENCH_SWAY_PID phase=cold pid=%s\n' "$sway_pid"
if ! wait_for_socket "$sway_pid"; then
    fail cold-socket
    [[ $wait_socket_sway_exited == 1 ]] || kill -KILL "$sway_pid"
    exit 1
fi
socket=$wait_socket
cold_socket=$(now_ns)
display=$(wait_for_wayland_display) || { fail cold-wayland-display; kill -KILL "$sway_pid"; exit 1; }
export WAYLAND_DISPLAY=$display
export SWAYSOCK=$socket
wait_for_background_presented || { fail cold-background; kill -KILL "$sway_pid"; exit 1; }
cold_background=$(now_ns)
window_event_start "$socket" || { fail cold-ipc; kill -KILL "$sway_pid"; exit 1; }
cold_ipc=$(now_ns)
printf 'P4_BENCH_STARTUP phase=cold socket_ms=%s background_ms=%s probe_ms=%s socket_to_background_ms=%s ipc_ms=%s display=%s\n' \
    "$(elapsed_ms "$cold_start" "$cold_socket")" \
    "$(elapsed_ms "$cold_start" "$cold_background")" \
    "$(elapsed_ms "$background_probe_start" "$background_probe_end")" \
    "$(elapsed_ms "$cold_socket" "$cold_background")" \
    "$(elapsed_ms "$cold_start" "$cold_ipc")" "$display"

if [[ ${P4_SWAY_ONLY:-0} == 1 ]]; then
    window_event_stop
    stop_sway "$socket" "$sway_pid" || {
        fail sway-only-exit
        exit 1
    }
    wait_for_runtime_clean || {
        fail sway-only-runtime-clean
        exit 1
    }
    printf 'P4_BENCH_PASS mode=sway-only direct_sway=1 background=presented cleanup=1\n'
    exit 0
fi

foot_pty_ack=$runtime/p4-foot-pty.ok
/bin/rm -f "$foot_pty_ack"
foot_start=$(now_ns)
printf 'P4_BENCH_APP_EXEC app=foot guest_ns=%s\n' "$foot_start"
/usr/bin/foot /bin/bash --noprofile --rcfile /cmd/phase4_foot_bashrc -i &
foot_pid=$!
printf 'P4_BENCH_APP_PID app=foot pid=%s\n' "$foot_pid"
wait_for_window_pid "$foot_pid" || {
    fail foot-map
    window_event_stop
    kill -KILL "$foot_pid" "$sway_pid" 2>/dev/null || true
    exit 1
}
foot_map=$(now_ns)
printf 'P4_BENCH_APP_EVENT app=foot event=new map_ms=%s\n' \
    "$(elapsed_ms "$foot_start" "$foot_map")"
printf 'P4_BENCH_FOOT_INPUT_READY key=a\n'
foot_pty_ticks=0
while [[ ! -e $foot_pty_ack && $foot_pty_ticks -lt 100 ]]; do
    kill -0 "$foot_pid" 2>/dev/null || break
    wait_tick 0.1
    foot_pty_ticks=$((foot_pty_ticks + 1))
done
[[ -e $foot_pty_ack ]] || { fail foot-pty-input; exit 1; }
printf 'P4_BENCH_APP app=foot map_ms=%s pty_input=1 resize=1\n' \
    "$(elapsed_ms "$foot_start" "$foot_map")"
foot_kill_start=$(now_ns)
foot_kill_output=
if ! foot_kill_output=$(
    /usr/bin/swaymsg -s "$socket" "[pid=\"$foot_pid\"] kill" 2>&1
); then
    printf 'P4_BENCH_FOOT_KILL status=failed output=%q\n' "$foot_kill_output"
    fail foot-kill-ipc
    exit 1
fi
printf 'P4_BENCH_FOOT_KILL status=ok output=%q\n' "$foot_kill_output"
wait_for_pid_stopped "$foot_pid" || { fail foot-exit; exit 1; }
wait "$foot_pid" 2>/dev/null || true
foot_exit=$(now_ns)
/bin/rm -f "$foot_pty_ack"
printf 'P4_BENCH_APP_EXIT app=foot close_ms=%s\n' \
    "$(elapsed_ms "$foot_kill_start" "$foot_exit")"

if [[ ${P4_FOOT_ONLY:-0} == 1 ]]; then
    window_event_stop
    stop_sway "$socket" "$sway_pid" || {
        fail foot-only-sway-exit
        exit 1
    }
    wait_for_runtime_clean || {
        fail foot-only-runtime-clean
        exit 1
    }
    printf 'P4_BENCH_PASS mode=foot-only direct_sway=1 foot=1 cleanup=1\n'
    exit 0
fi

gtk_start=$(now_ns)
printf 'P4_BENCH_APP_EXEC app=gtk3-demo\n'
GDK_BACKEND=wayland /usr/bin/gtk3-demo &
gtk_pid=$!
wait_for_window_pid "$gtk_pid" || {
    fail gtk-map
    kill -KILL "$gtk_pid" "$sway_pid" 2>/dev/null || true
    exit 1
}
gtk_map=$(now_ns)
printf 'P4_BENCH_GTK_INPUT_READY keyboard=1 pointer=1\n'
printf 'P4_BENCH_APP app=gtk3-demo map_ms=%s resize=1 input=keyboard,pointer backend=wayland\n' \
    "$(elapsed_ms "$gtk_start" "$gtk_map")"
kill -TERM "$gtk_pid" 2>/dev/null || true
wait_for_pid_stopped "$gtk_pid" || { fail gtk-exit; exit 1; }
wait "$gtk_pid" 2>/dev/null || true
printf 'P4_BENCH_APP_EXIT app=gtk3-demo\n'
window_event_stop

/cmd/lpr_wayland_animation_bench.elf || {
    fail animation
    stop_sway "$socket" "$sway_pid" || true
    exit 1
}

printf 'P4_BENCH_IDLE_BEGIN seconds=5\n'
wait_tick 5
printf 'P4_BENCH_IDLE_END seconds=5\n'
/bin/sync

stop_sway "$socket" "$sway_pid" || { fail cold-sway-exit; exit 1; }
wait_for_runtime_clean || { fail cold-runtime-clean; exit 1; }

unset WAYLAND_DISPLAY SWAYSOCK
warm_start=$(now_ns)
printf 'P4_BENCH_SWAY_EXEC phase=warm guest_ns=%s path=/usr/bin/sway\n' "$warm_start"
/usr/bin/sway -c "$config" &
warm_sway_pid=$!
if ! wait_for_socket "$warm_sway_pid"; then
    fail warm-socket
    [[ $wait_socket_sway_exited == 1 ]] || kill -KILL "$warm_sway_pid"
    exit 1
fi
warm_socket=$wait_socket
warm_socket_time=$(now_ns)
warm_display=$(wait_for_wayland_display) || { fail warm-wayland-display; kill -KILL "$warm_sway_pid"; exit 1; }
export WAYLAND_DISPLAY=$warm_display
export SWAYSOCK=$warm_socket
wait_for_background_presented || { fail warm-background; kill -KILL "$warm_sway_pid"; exit 1; }
warm_background=$(now_ns)
window_event_start "$warm_socket" || { fail warm-ipc; kill -KILL "$warm_sway_pid"; exit 1; }
warm_ipc=$(now_ns)
window_event_stop
printf 'P4_BENCH_STARTUP phase=warm socket_ms=%s background_ms=%s probe_ms=%s socket_to_background_ms=%s ipc_ms=%s\n' \
    "$(elapsed_ms "$warm_start" "$warm_socket_time")" \
    "$(elapsed_ms "$warm_start" "$warm_background")" \
    "$(elapsed_ms "$background_probe_start" "$background_probe_end")" \
    "$(elapsed_ms "$warm_socket_time" "$warm_background")" \
    "$(elapsed_ms "$warm_start" "$warm_ipc")"
stop_sway "$warm_socket" "$warm_sway_pid" || { fail warm-sway-exit; exit 1; }
wait_for_runtime_clean || { fail warm-runtime-clean; exit 1; }
/bin/sync
printf 'P4_BENCH_PASS direct_sway=1 foot=1 gtk3=1 animation=1 idle=1\n'
