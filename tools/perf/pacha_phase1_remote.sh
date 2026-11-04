#!/bin/sh
set -eu

mode=${1:-all}
run_root=/tmp/pacha-perf/results
x11=/tmp/pacha-perf/x11_probe
mc_dir=/root/.local/share/PrismLauncher/instances/CapabilityOS-26.1.2/minecraft
latest=$mc_dir/logs/latest.log
mkdir -p "$run_root"
export DISPLAY=:0
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/0/bus

x11_run() {
    case "$1" in
        focus|key|shot)
            LD_LIBRARY_PATH=/opt/java25-openj9/glibc:/usr/lib:/lib timeout 30 "$x11" "$@"
            ;;
        *)
            LD_LIBRARY_PATH=/opt/java25-openj9/glibc:/usr/lib:/lib "$x11" "$@"
            ;;
    esac
}

wait_log() {
    pattern=$1
    timeout_seconds=$2
    elapsed=0
    while [ "$elapsed" -lt "$timeout_seconds" ]; do
        if test -f "$latest" && grep -Fq "$pattern" "$latest"; then
            return 0
        fi
        sleep 2
        elapsed=$((elapsed + 2))
    done
    return 1
}

run_terminal() {
    output=$run_root/terminal
    mkdir -p "$output"
    for run in 1 2 3; do
        title=PACHA_PERF_TERM_$run
        x11_run run-wait "$title" 30000 -- /usr/bin/env -u LD_LIBRARY_PATH \
            /usr/bin/xfce4-terminal --disable-server --dynamic-title-mode=none \
            --title="$title" --command="/bin/sh -c 'sleep 2'" \
            > "$output/start-$run.txt" 2> "$output/start-$run.err"
        sleep 3
    done
    for input_name in seq-200000.txt block-8m.txt; do
        for run in 1 2 3; do
            marker=/tmp/pacha-perf/terminal-$input_name-$run.done
            title=PACHA_PERF_OUT_${input_name}_$run
            x11_run run-wait-file "$marker" 300000 -- /usr/bin/env -u LD_LIBRARY_PATH \
                /usr/bin/xfce4-terminal --disable-server --dynamic-title-mode=none \
                --title="$title" --command="/bin/sh /tmp/pacha-perf/pacha_phase1_terminal_payload.sh $marker /tmp/pacha-perf/$input_name" \
                > "$output/output-$input_name-$run.txt" 2> "$output/output-$input_name-$run.err"
            sleep 2
        done
    done
}

run_minecraft() {
    run=$1
    instrument=${2:-0}
    output=$run_root/minecraft-$run
    mkdir -p "$output"
    if pgrep -x java >/dev/null 2>&1 || pgrep -x prismlauncher >/dev/null 2>&1; then
        echo "Minecraft is already running" >&2
        return 2
    fi
    if test -f "$latest"; then mv "$latest" "$output/pre-existing-latest.log"; fi
    cp /proc/stat "$output/proc-stat-before.txt"
    cp /proc/meminfo "$output/meminfo-before.txt"
    wc -c /run/pacha/kernel.log /run/pacha/service.log > "$output/log-offsets-before.txt"
    start_epoch=$(date +%s)
    echo "$start_epoch" > "$output/start-epoch.txt"
    prism="$output/prism-console.log"
    config_modified=0
    instance_cfg=/root/.local/share/PrismLauncher/instances/CapabilityOS-26.1.2/instance.cfg
    restore_instance_config() {
        if [ "$config_modified" = 1 ]; then
            cp "$output/instance.cfg.before" "$instance_cfg"
            config_modified=0
        fi
    }
    trap 'restore_instance_config' 0 1 2 15
    if [ "$instrument" = 1 ]; then
        agent="$output/agent.tsv"
        gc="$output/verbosegc.xml"
        cp "$instance_cfg" "$output/instance.cfg.before"
        # Prism deliberately strips _JAVA_OPTIONS. These are the launcher's
        # native per-instance keys; restore the exact original after exit.
        jvm_args="-javaagent:/tmp/pacha-perf/pacha-perf-agent.jar=$agent -Xverbosegclog:$gc"
        awk -v jvm_args="$jvm_args" '
            $0 == "[UI]" { print "OverrideJavaArgs=true"; print "JvmArgs=" jvm_args }
            { print }
        ' "$instance_cfg" > "$instance_cfg.pacha-perf"
        mv "$instance_cfg.pacha-perf" "$instance_cfg"
        config_modified=1
    fi
    # This image currently opens Prism's language quick-setup dialog on every
    # fresh process. Finish the existing dialog without changing global config.
    (
        if x11_run wait 'Prism Launcher Quick Setup' 30000 >/dev/null 2>&1; then
            x11_run focus 'Prism Launcher Quick Setup'
            x11_run key Return
        fi
    ) > "$output/quick-setup.txt" 2> "$output/quick-setup.err" &
    x11_run run-wait 'Minecraft' 600000 -- /bin/sh -c \
        "exec /usr/local/bin/prismlauncher --launch CapabilityOS-26.1.2 --world 'New World' >'$prism' 2>&1" \
        > "$output/window-map.txt" 2> "$output/window-map.err"
    map_epoch=$(date +%s)
    echo "$map_epoch" > "$output/map-epoch.txt"
    if ! wait_log 'Started 2 worker threads' 600; then
        echo "world ready timeout" > "$output/failure.txt"
        return 1
    fi
    ready_epoch=$(date +%s)
    echo "$ready_epoch" > "$output/ready-epoch.txt"
    sleep 15
    java_pid=$(pgrep -x java | head -1)
    if [ -n "$java_pid" ]; then
        kill -QUIT "$java_pid"
        sleep 12
        javacore=$(ls -t "$mc_dir"/javacore.*.txt 2>/dev/null | head -1 || true)
        if [ -n "$javacore" ]; then
            printf '%s\n' "$javacore" > "$output/javacore-path.txt"
            cp "$javacore" "$output/javacore.txt"
        fi
    fi
    sleep 3
    x11_run focus 'Minecraft'
    x11_run key F3
    sleep 8
    x11_run shot 'Minecraft' "$output/f3.ppm"
    x11_run key F3
    sleep 1
    x11_run key Escape
    sleep 2
    x11_run shot 'Minecraft' "$output/pause.ppm"
    x11_run key Shift_L down
    x11_run key Tab
    x11_run key Shift_L up
    sleep 1
    x11_run key Return
    if ! wait_log 'Stopping singleplayer server' 90; then
        echo "normal exit first attempt failed" >> "$output/failure.txt"
        x11_run focus 'Minecraft' || true
        x11_run key Escape || true
        sleep 1
        x11_run key Shift_L down || true
        x11_run key Tab || true
        x11_run key Shift_L up || true
        x11_run key Return || true
    fi
    elapsed=0
    while { pgrep -x java >/dev/null 2>&1 || pgrep -x prismlauncher >/dev/null 2>&1; } && [ "$elapsed" -lt 120 ]; do
        sleep 2
        elapsed=$((elapsed + 2))
    done
    if pgrep -x java >/dev/null 2>&1 || pgrep -x prismlauncher >/dev/null 2>&1; then
        echo "normal shutdown exceeded 120 seconds; sending Alt-F4" >> "$output/failure.txt"
        x11_run focus 'Minecraft' || true
        x11_run key Alt_L down || true
        x11_run key F4 || true
        x11_run key Alt_L up || true
        sleep 30
    fi
    if pgrep -x java >/dev/null 2>&1; then
        if grep -Fq 'ThreadedAnvilChunkStorage: All dimensions are saved' "$latest"; then
            echo "saved world but JVM remained; sent SIGTERM" >> "$output/failure.txt"
            pkill -TERM -x java
            sleep 10
        else
            echo "JVM remained and world-save completion was not observed" >> "$output/failure.txt"
            return 1
        fi
    fi
    restore_instance_config
    trap - 0 1 2 15
    end_epoch=$(date +%s)
    echo "$end_epoch" > "$output/end-epoch.txt"
    cp "$latest" "$output/latest.log"
    cp /proc/stat "$output/proc-stat-after.txt"
    cp /proc/meminfo "$output/meminfo-after.txt"
    wc -c /run/pacha/kernel.log /run/pacha/service.log > "$output/log-offsets-after.txt"
}

case "$mode" in
    terminal) run_terminal ;;
    minecraft) run_minecraft "${2:?run number required}" "${3:-0}" ;;
    all)
        run_terminal
        run_minecraft 1 0
        ;;
    *) echo "usage: $0 terminal|minecraft RUN|all" >&2; exit 2 ;;
esac
