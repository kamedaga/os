#!/bin/bash

set -u

runtime=/run/user/0
watchdog_seconds=${SWAY_PHASE1_WATCHDOG_SECONDS:-20}
cleanup_ticks=${SWAY_PHASE1_CLEANUP_TICKS:-300}

fail()
{
    if [[ -n ${active_sway_log:-} && -f $active_sway_log ]]; then
        printf 'SWAY_PHASE1_DIAGNOSTIC_BEGIN iteration=%s log=%s\n' \
            "${i:-unknown}" "$active_sway_log"
        /bin/cat "$active_sway_log"
        printf 'SWAY_PHASE1_DIAGNOSTIC_END iteration=%s\n' "${i:-unknown}"
    fi
    printf 'SWAY_PHASE1_ACCEPTANCE_FAIL stage=%s\n' "$1"
    return 1
}

wait_tick()
{
    IFS= read -r -t "$1" -n 1 _ || true
}

runtime_is_clean()
{
    shopt -s nullglob
    local stale=(
        "$runtime"/wayland-*
        "$runtime"/sway-ipc.*.sock
    )
    shopt -u nullglob
    [[ ${#stale[@]} -eq 0 ]]
}

wait_for_runtime_clean()
{
    local ticks=0
    while ! runtime_is_clean && [[ $ticks -lt $cleanup_ticks ]]; do
        wait_tick 0.1
        ticks=$((ticks + 1))
    done
    runtime_is_clean
}

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

wait_for_ipc_ready()
{
    local socket=$1
    local ticks=0
    while [[ $ticks -lt 1200 ]]; do
        if /usr/bin/swaymsg -s "$socket" -t get_version >/dev/null 2>&1; then
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
    local iteration=$2
    local pattern="SWAY_PHASE1_FOOT_CHILD iteration=$iteration pty=1 environment=clean child_pid=([0-9]+) foot_pid=([0-9]+)"
    local ticks=0
    while [[ $ticks -lt 1200 ]]; do
        local title
        # Keep the multi-kilobyte tree out of Bash's command-substitution buffer.
        title=$(/usr/bin/swaymsg -s "$socket" -t get_tree 2>/dev/null |
            /bin/grep -Eom1 "$pattern" || true)
        if [[ $title =~ $pattern ]]; then
            printf '%s %s\n' "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}"
            return 0
        fi
        wait_tick 0.1
        ticks=$((ticks + 1))
    done
    return 1
}

wait_for_processes_gone()
{
    local child_pid=$1
    local foot_pid=$2
    local ticks=0
    local child_alive=0
    local foot_alive=0
    while [[ $ticks -lt $cleanup_ticks ]]; do
        child_alive=0
        foot_alive=0
        kill -0 "$child_pid" 2>/dev/null && child_alive=1
        kill -0 "$foot_pid" 2>/dev/null && foot_alive=1
        if [[ $child_alive -eq 0 && $foot_alive -eq 0 ]]; then
            return 0
        fi
        wait_tick 0.1
        ticks=$((ticks + 1))
    done
    printf 'SWAY_PHASE1_PROCESS_REMAINS child_alive=%s foot_alive=%s\n' \
        "$child_alive" "$foot_alive"
    return 1
}

cleanup_killed_runtime()
{
    shopt -s nullglob
    local stale=(
        "$runtime"/wayland-*
        "$runtime"/sway-ipc.*.sock
    )
    shopt -u nullglob
    if [[ ${#stale[@]} -ne 0 ]]; then
        rm -f -- "${stale[@]}"
    fi
    printf 'SWAY_PHASE1_KILL_STALE_CLEANED iteration=%s entries=%s\n' \
        "$i" "${#stale[@]}"
}

resource_probe()
{
    local stage=$1
    /bin/sync &
    local probe_pid=$!
    printf 'SWAY_PHASE1_LPR_PROBE stage=%s\n' "$stage"
    printf 'SWAY_PHASE1_FILED_PROBE stage=%s\n' "$stage"
    wait "$probe_pid" || return 1
}

if [[ ${1:-} == --foot-child ]]; then
    iteration=${SWAY_PHASE1_ITERATION:?missing iteration}
    pty=0
    environment=clean
    if [[ -t 0 && -t 1 && -t 2 ]]; then
        pty=1
    fi
    if [[ ${LD_PRELOAD+x} || ${LP_NUM_THREADS+x} ||
          ${M57_WLROOTS_KEYMAP_PRELOAD+x} ]]; then
        environment=dirty
    fi
    printf '\033]0;SWAY_PHASE1_FOOT_CHILD iteration=%s pty=%s environment=%s child_pid=%s foot_pid=%s\007' \
        "$iteration" "$pty" "$environment" "$$" "$PPID"
    trap 'exit 0' HUP INT TERM
    while :; do
        wait_tick 3600
    done
fi

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
if ! runtime_is_clean; then
    fail stale-runtime-at-baseline
    exit 1
fi

printf 'SWAY_PHASE1_SESSION_OK runtime=%s seatd=persistent environment=clean\n' "$runtime"
resource_probe baseline || {
    fail sync-baseline
    exit 1
}

iterations=${SWAY_PHASE1_ITERATIONS:-5}
i=${SWAY_PHASE1_START_ITERATION:-1}
active_sway_log=
while [[ $i -le $iterations ]]; do
    case $i in
        3|8) mode=term ;;
        5|10) mode=kill ;;
        *) mode=normal ;;
    esac

    printf 'SWAY_PHASE1_ITERATION_BEGIN iteration=%s mode=%s path=/usr/bin/sway\n' "$i" "$mode"
    export SWAY_PHASE1_ITERATION=$i
    if [[ $i -eq 1 || ${SWAY_PHASE1_DEBUG_ALL:-0} == 1 ]]; then
        active_sway_log=
        /usr/bin/sway -d -c /cmd/sway_phase1_acceptance.conf &
    else
        active_sway_log="$runtime/sway-phase1-iteration-$i.log"
        rm -f -- "$active_sway_log"
        /usr/bin/sway -c /cmd/sway_phase1_acceptance.conf >"$active_sway_log" 2>&1 &
    fi
    sway_pid=$!

    socket=$(wait_for_socket) || {
        fail "sway-socket-$i"
        exit 1
    }
    if ! wait_for_ipc_ready "$socket"; then
        fail "sway-ipc-ready-$i"
        exit 1
    fi
    foot_pids=$(wait_for_foot_pids "$socket" "$i") || {
        fail "foot-title-$i"
        exit 1
    }
    child_pid=${foot_pids%% *}
    foot_pid=${foot_pids#* }
    if ! [[ $child_pid =~ ^[0-9]+$ && $foot_pid =~ ^[0-9]+$ ]]; then
        fail "foot-pids-$i"
        exit 1
    fi
    # The title proves the Foot PTY is live; leave one real render interval
    # before publishing the marker used by the host screendump check.
    /bin/sleep 2
    printf 'SWAY_PHASE1_FOOT_OK iteration=%s pty=1 environment=clean child_pid=%s foot_pid=%s\n' \
        "$i" "$child_pid" "$foot_pid"
    printf 'SWAY_PHASE1_FRAME_READY iteration=%s mode=%s sway_pid=%s foot_pid=%s\n' \
        "$i" "$mode" "$sway_pid" "$foot_pid"
    if [[ $i -eq 1 ]]; then
        printf 'SWAY_PHASE1_RENDERER threaded_llvmpipe=1\n'
    fi

    watchdog=
    case $mode in
        normal)
            /usr/bin/swaymsg -s "$socket" kill >/dev/null 2>&1 || true
            /usr/bin/swaymsg -s "$socket" exit >/dev/null 2>&1 || true
            ;;
        term)
            kill -TERM "$sway_pid" || { fail "term-signal-$i"; exit 1; }
            ;;
        kill)
            kill -KILL "$sway_pid" || { fail "kill-signal-$i"; exit 1; }
            ;;
    esac
    if [[ $mode != kill ]]; then
        (
            timer_pid=
            stop_watchdog()
            {
                if [[ -n $timer_pid ]]; then
                    kill "$timer_pid" 2>/dev/null || true
                    wait "$timer_pid" 2>/dev/null || true
                fi
                exit 0
            }
            trap stop_watchdog TERM INT
            /bin/sleep "$watchdog_seconds" &
            timer_pid=$!
            wait "$timer_pid" 2>/dev/null || exit 0
            timer_pid=
            if kill -KILL "$sway_pid" 2>/dev/null; then
                printf 'SWAY_PHASE1_WATCHDOG_ESCALATED iteration=%s mode=%s\n' "$i" "$mode"
            fi
        ) &
        watchdog=$!
    fi

    wait "$sway_pid"
    sway_status=$?
    if [[ -n $watchdog ]]; then
        kill "$watchdog" 2>/dev/null || true
        wait "$watchdog" 2>/dev/null || true
    fi
    if ! wait_for_processes_gone "$child_pid" "$foot_pid"; then
        fail "process-clean-$i"
        exit 1
    fi
    if [[ $mode == kill ]]; then
        # SIGKILL cannot run Sway's pathname unlink handlers.  The parent
        # session owns recovery of those known-stale runtime entries.
        cleanup_killed_runtime
    fi
    if ! wait_for_runtime_clean; then
        fail "runtime-clean-$i"
        exit 1
    fi
    case $mode:$sway_status in
        normal:0|term:0|term:143|kill:137) ;;
        *) fail "exit-status-$i-$sway_status"; exit 1 ;;
    esac

    printf 'SWAY_PHASE1_LIFECYCLE_OK iteration=%s mode=%s sway_status=%s processes=gone sockets=gone\n' \
        "$i" "$mode" "$sway_status"
    if [[ -n $active_sway_log ]]; then
        rm -f -- "$active_sway_log"
        active_sway_log=
    fi
    resource_probe "iteration-$i" || {
        fail "sync-$i"
        exit 1
    }
    i=$((i + 1))
done

printf 'SWAY_PHASE1_ACCEPTANCE_PASS iterations=%s lifecycle=normal,term,kill foot_pty=1 environment=clean\n' \
    "$iterations"
