#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ssh_target=${PACHA_PERF_SSH_TARGET:-root@192.168.3.26}
ssh_key=${PACHA_PERF_SSH_KEY:-$HOME/.ssh/pachaos_live_ed25519}
out="$repo_root/.artifacts/perf/20261102-phase2"
mkdir -p "$out"
"$repo_root/tools/perf/build_pacha_perf_helpers.sh"
ssh_cmd=(ssh -i "$ssh_key" "$ssh_target")
deploy() {
    local source=$1 destination=$2 mode=$3
    "${ssh_cmd[@]}" "dd of='$destination' bs=65536 status=none; chmod '$mode' '$destination'" < "$source"
}
"${ssh_cmd[@]}" 'mkdir -p /tmp/pacha-perf'
deploy "$repo_root/.artifacts/perf/helpers/x11_probe" /tmp/pacha-perf/x11_probe 755
deploy "$repo_root/.artifacts/perf/helpers/pacha-perf-agent.jar" /tmp/pacha-perf/pacha-perf-agent.jar 644
deploy "$repo_root/tools/perf/pacha_phase2_remote.sh" /tmp/pacha-perf/pacha_phase2_remote.sh 755
"${ssh_cmd[@]}" '/tmp/pacha-perf/pacha_phase2_remote.sh' | tee "$out/remote-driver.log"
mkdir -p "$out/remote"
"${ssh_cmd[@]}" 'cd /tmp/pacha-perf/phase2-results && tar -cf - .' | tar -xf - -C "$out/remote"
printf 'Results: %s\n' "$out"
