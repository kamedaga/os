#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

container=${LINUX_GUI_CONTAINER:-capabilityos-linux-gui-bench}
iterations=${1:-5}
target=${2:-all}
out_dir=.artifacts/test-results/gui-startup/linux-sandboxless
out_file="$out_dir/run-$(date +%Y%m%d-%H%M%S).log"

mkdir -p "$out_dir"

if ! docker inspect "$container" >/dev/null 2>&1; then
    printf 'missing prepared container: %s\n' "$container" >&2
    printf 'prepare it from alpine:latest with sway foot gedit jq coreutils\n' >&2
    exit 1
fi
if [ "$(docker inspect -f '{{.State.Running}}' "$container")" != true ]; then
    docker start "$container" >/dev/null
fi

if ! docker exec "$container" sh -c \
    'command -v setcap >/dev/null && command -v swaybg >/dev/null && fc-match monospace | grep -q . && date +%s%N | grep -Eq "^[0-9]+$"'; then
    docker exec "$container" apk add --no-cache \
        coreutils libcap-utils swaybg font-dejavu >/dev/null
fi
if [ "$target" = thunar ] && ! docker exec "$container" sh -c \
    'command -v thunar >/dev/null && command -v dbus-daemon >/dev/null'; then
    docker exec "$container" apk add --no-cache thunar dbus >/dev/null
fi
# Alpine grants sway cap_sys_nice. Docker's default capability bounding set does
# not, so execve rejects the binary before wlroots starts. The headless
# benchmark does not need realtime scheduling.
docker exec "$container" setcap -r /usr/bin/sway 2>/dev/null || true
docker exec "$container" mkdir -p /opt/capabilityos-bench
docker cp tests/fixtures/linux_sandboxless_gui_benchmark.sh \
    "$container":/opt/capabilityos-bench/benchmark.sh >/dev/null
docker cp tests/fixtures/linux_sandboxless_sway.conf \
    "$container":/opt/capabilityos-bench/linux_sandboxless_sway.conf >/dev/null
docker exec "$container" chmod +x /opt/capabilityos-bench/benchmark.sh
if [ "$target" = glycin-app-png-3 ]; then
    probe=.artifacts/userland-fixtures/lpr_glycin_app_probe.elf
    if [ ! -x "$probe" ]; then
        printf 'missing Linux Glycin probe: %s\n' "$probe" >&2
        exit 1
    fi
    docker cp "$probe" "$container":/usr/local/bin/glycin-app-probe >/dev/null
    docker exec "$container" chmod +x /usr/local/bin/glycin-app-probe
fi

docker exec --env LINUX_GUI_STRACE="${LINUX_GUI_STRACE:-0}" \
    "$container" /opt/capabilityos-bench/benchmark.sh \
    "$iterations" "$target" | tee "$out_file"
if [ "${LINUX_GUI_STRACE:-0}" = 1 ]; then
    trace_dir="${out_file%.log}-traces"
    docker cp "$container":/tmp/capabilityos-gui-logs "$trace_dir" >/dev/null
    printf 'Linux syscall traces: %s\n' "$trace_dir"
fi
printf 'Linux sandboxless GUI result: %s\n' "$out_file"
