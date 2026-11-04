#!/bin/sh
set -eu

run_root=/tmp/pacha-perf/phase2-results
x11=/tmp/pacha-perf/x11_probe
mc_dir=/root/.local/share/PrismLauncher/instances/CapabilityOS-26.1.2/minecraft
latest=$mc_dir/logs/latest.log
output=$run_root/minecraft
mkdir -p "$output"
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

live_pid() {
    ps -eo pid,stat,comm | awk -v wanted="$1" '
        NR > 1 && $2 !~ /^Z/ && $3 == wanted { print $1; exit }
    '
}

live_named() {
    test -n "$(live_pid "$1")"
}

wait_log() {
    pattern=$1
    timeout_seconds=$2
    elapsed=0
    while [ "$elapsed" -lt "$timeout_seconds" ]; do
        if test -f "$latest" && grep -Fq "$pattern" "$latest"; then return 0; fi
        sleep 2
        elapsed=$((elapsed + 2))
    done
    return 1
}

if live_named java || live_named prismlauncher; then
    echo "Minecraft is already running" >&2
    exit 2
fi
rm -rf "$run_root"
mkdir -p "$output/javacores"
if test -f "$latest"; then mv "$latest" "$output/pre-existing-latest.log"; fi
touch "$output/run-start.marker"
cp /proc/stat "$output/proc-stat-before.txt"
cp /proc/meminfo "$output/meminfo-before.txt"
wc -c /run/pacha/kernel.log /run/pacha/service.log > "$output/log-offsets-before.txt"
date +%s > "$output/start-epoch.txt"

instance_cfg=/root/.local/share/PrismLauncher/instances/CapabilityOS-26.1.2/instance.cfg
cp "$instance_cfg" "$output/instance.cfg.before"
config_modified=1
restore_instance_config() {
    if [ "$config_modified" = 1 ]; then
        cp "$output/instance.cfg.before" "$instance_cfg"
        config_modified=0
    fi
}
trap 'restore_instance_config' 0 1 2 15
jvm_args="-javaagent:/tmp/pacha-perf/pacha-perf-agent.jar=$output/agent.tsv"
awk -v jvm_args="$jvm_args" '
    $0 == "[UI]" { print "OverrideJavaArgs=true"; print "JvmArgs=" jvm_args }
    { print }
' "$instance_cfg" > "$instance_cfg.pacha-perf"
mv "$instance_cfg.pacha-perf" "$instance_cfg"

(
    if x11_run wait 'Prism Launcher Quick Setup' 30000 >/dev/null 2>&1; then
        x11_run focus 'Prism Launcher Quick Setup'
        x11_run key Return
    fi
) > "$output/quick-setup.txt" 2> "$output/quick-setup.err" &

prism=$output/prism-console.log
x11_run run-wait 'Minecraft' 600000 -- /bin/sh -c \
    "exec /usr/local/bin/prismlauncher --launch CapabilityOS-26.1.2 --offline Phase2Probe --world 'New World' >'$prism' 2>&1" \
    > "$output/window-map.txt" 2> "$output/window-map.err"
date +%s > "$output/map-epoch.txt"

if ! wait_log 'Started 2 worker threads' 600; then
    echo "world ready timeout" > "$output/failure.txt"
    exit 1
fi
date +%s > "$output/ready-epoch.txt"
sleep 10
java_pid=$(live_pid java)
if [ -n "$java_pid" ]; then
    kill -QUIT "$java_pid"
    sleep 8
fi

x11_run focus 'Minecraft'
x11_run key Escape
sleep 2
x11_run key Shift_L down
x11_run key Tab
x11_run key Shift_L up
sleep 1
x11_run key Return
if ! wait_log 'Stopping worker threads' 120; then
    echo "shutdown start timeout" >> "$output/failure.txt"
fi
date +%s > "$output/shutdown-wait-start-epoch.txt"

# Capture the Java/native state while the phase-1 127 second wait is active.
sleep 20
java_pid=$(live_pid java)
if [ -n "$java_pid" ]; then
    kill -QUIT "$java_pid"
fi

elapsed=20
while live_named java && [ "$elapsed" -lt 210 ]; do
    sleep 2
    elapsed=$((elapsed + 2))
done
printf '%s\n' "$elapsed" > "$output/shutdown-wait-observed-seconds.txt"
if live_named java; then
    echo "normal shutdown exceeded 210 seconds" >> "$output/failure.txt"
    if grep -Fq 'ThreadedAnvilChunkStorage: All dimensions are saved' "$latest"; then
        pkill -TERM -x java || true
        sleep 10
    else
        exit 1
    fi
fi

# Prism normally remains open after the game has exited.  It is outside the
# measured Minecraft shutdown interval; close it normally so run-wait can
# return and the exact instance configuration can be restored.
if live_named prismlauncher; then
    x11_run focus 'Prism Launcher' || true
    x11_run key Alt_L down || true
    x11_run key F4 || true
    x11_run key Alt_L up || true
    waited=0
    while live_named prismlauncher && [ "$waited" -lt 30 ]; do
        sleep 2
        waited=$((waited + 2))
    done
fi

find "$mc_dir" -maxdepth 1 -type f -name 'javacore.*.txt' \
    -newer "$output/run-start.marker" -exec cp '{}' "$output/javacores/" ';'
restore_instance_config
trap - 0 1 2 15
date +%s > "$output/end-epoch.txt"
cp "$latest" "$output/latest.log"
cp /proc/stat "$output/proc-stat-after.txt"
cp /proc/meminfo "$output/meminfo-after.txt"
wc -c /run/pacha/kernel.log /run/pacha/service.log > "$output/log-offsets-after.txt"
