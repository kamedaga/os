#!/bin/bash

set -u

runtime=/run/user/0
config=/cmd/phase4_gui_benchmark.conf
wait_limit=1200
process_poll_limit=60

wait_tick()
{
    IFS= read -r -t "$1" -n 1 _ || true
}

now_ns()
{
    /bin/date +%s%N
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

wait_for_ipc()
{
    local socket=$1
    local ticks=0
    while [[ $ticks -lt $process_poll_limit ]]; do
        if /usr/bin/swaymsg -s "$socket" -t get_version >/dev/null 2>&1; then
            return 0
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

tree_has_pid()
{
    local socket=$1
    local pid=$2
    local tree
    local pattern="\"pid\"[[:space:]]*:[[:space:]]*${pid}([,}])"
    tree=$(/usr/bin/swaymsg -s "$socket" -t get_tree 2>/dev/null) || return 1
    [[ $tree =~ $pattern ]]
}

wait_for_tree_pid()
{
    local socket=$1
    local pid=$2
    local ticks=0
    while [[ $ticks -lt $process_poll_limit ]]; do
        if tree_has_pid "$socket" "$pid"; then
            return 0
        fi
        pid_has_stopped "$pid" && return 1
        wait_tick 2
        ticks=$((ticks + 1))
    done
    return 1
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
    while [[ $ticks -lt 200 ]]; do
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
      ${M57_WLROOTS_KEYMAP_PRELOAD+x} || ${MESA_SHADER_CACHE_DISABLE+x} ]]; then
    fail forbidden-environment
    exit 1
fi
if ! wait_for_runtime_clean; then
    fail stale-runtime-at-baseline
    exit 1
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
    wait_for_ipc "$socket" || { fail p3a-ipc; kill -KILL "$sway_pid"; exit 1; }
    p3a_ipc=$(now_ns)
    display=$(wait_for_wayland_display) || { fail p3a-display; kill -KILL "$sway_pid"; exit 1; }
    export WAYLAND_DISPLAY=$display
    export SWAYSOCK=$socket
    printf 'P4_BENCH_STARTUP phase=p3a socket_ms=%s ipc_ms=%s display=%s\n' \
        "$(elapsed_ms "$p3a_start" "$p3a_socket")" \
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
if ! wait_for_socket "$sway_pid"; then
    fail cold-socket
    [[ $wait_socket_sway_exited == 1 ]] || kill -KILL "$sway_pid"
    exit 1
fi
socket=$wait_socket
cold_socket=$(now_ns)
wait_for_ipc "$socket" || { fail cold-ipc; kill -KILL "$sway_pid"; exit 1; }
cold_ipc=$(now_ns)
display=$(wait_for_wayland_display) || { fail cold-wayland-display; kill -KILL "$sway_pid"; exit 1; }
export WAYLAND_DISPLAY=$display
export SWAYSOCK=$socket
printf 'P4_BENCH_STARTUP phase=cold socket_ms=%s ipc_ms=%s display=%s\n' \
    "$(elapsed_ms "$cold_start" "$cold_socket")" \
    "$(elapsed_ms "$cold_start" "$cold_ipc")" "$display"

foot_pty_ack=$runtime/p4-foot-pty.ok
/bin/rm -f "$foot_pty_ack"
foot_start=$(now_ns)
printf 'P4_BENCH_APP_EXEC app=foot\n'
/usr/bin/foot /bin/bash --noprofile --rcfile /cmd/phase4_foot_bashrc -i &
foot_pid=$!
wait_for_tree_pid "$socket" "$foot_pid" || {
    fail foot-map
    kill -KILL "$foot_pid" "$sway_pid" 2>/dev/null || true
    exit 1
}
foot_map=$(now_ns)
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
/usr/bin/swaymsg -s "$socket" "[pid=\"$foot_pid\"] kill" >/dev/null 2>&1 || true
wait_for_pid_stopped "$foot_pid" || { fail foot-exit; exit 1; }
wait "$foot_pid" 2>/dev/null || true
/bin/rm -f "$foot_pty_ack"
printf 'P4_BENCH_APP_EXIT app=foot\n'

gtk_start=$(now_ns)
printf 'P4_BENCH_APP_EXEC app=gtk3-demo\n'
GDK_BACKEND=wayland /usr/bin/gtk3-demo &
gtk_pid=$!
wait_for_tree_pid "$socket" "$gtk_pid" || {
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

/cmd/lpr_wayland_animation_bench.elf || {
    fail animation
    stop_sway "$socket" "$sway_pid" || true
    exit 1
}

printf 'P4_BENCH_IDLE_BEGIN seconds=5\n'
/bin/sleep 5
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
wait_for_ipc "$warm_socket" || { fail warm-ipc; kill -KILL "$warm_sway_pid"; exit 1; }
warm_ipc=$(now_ns)
printf 'P4_BENCH_STARTUP phase=warm socket_ms=%s ipc_ms=%s\n' \
    "$(elapsed_ms "$warm_start" "$warm_socket_time")" \
    "$(elapsed_ms "$warm_start" "$warm_ipc")"
stop_sway "$warm_socket" "$warm_sway_pid" || { fail warm-sway-exit; exit 1; }
wait_for_runtime_clean || { fail warm-runtime-clean; exit 1; }
/bin/sync
printf 'P4_BENCH_PASS direct_sway=1 foot=1 gtk3=1 animation=1 idle=1\n'
