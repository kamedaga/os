#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ssh_target=${PACHA_PERF_SSH_TARGET:-root@192.168.3.26}
ssh_key=${PACHA_PERF_SSH_KEY:-$HOME/.ssh/pachaos_live_ed25519}
stamp=${PACHA_PERF_STAMP:-$(date -u +%Y%m%dT%H%M%SZ)}
mode=${PACHA_PERF_MODE:-all}
instrument=${PACHA_PERF_INSTRUMENT:-0}
case "$mode" in
    all|terminal) ;;
    minecraft) ;;
    *) printf 'PACHA_PERF_MODE must be all, terminal, or minecraft\n' >&2; exit 2 ;;
esac
case "$instrument" in
    0|1) ;;
    *) printf 'PACHA_PERF_INSTRUMENT must be 0 or 1\n' >&2; exit 2 ;;
esac
out="$repo_root/.artifacts/perf/$stamp"
mkdir -p "$out"
"$repo_root/tools/perf/build_pacha_perf_helpers.sh"
ssh_cmd=(ssh -i "$ssh_key" "$ssh_target")
"${ssh_cmd[@]}" 'mkdir -p /tmp/pacha-perf/results'
deploy() {
    local source=$1 destination=$2 mode=$3
    "${ssh_cmd[@]}" "dd of='$destination' bs=65536 status=none; chmod '$mode' '$destination'" < "$source"
}
deploy "$repo_root/.artifacts/perf/helpers/x11_probe" /tmp/pacha-perf/x11_probe 755
deploy "$repo_root/.artifacts/perf/helpers/pacha-perf-agent.jar" /tmp/pacha-perf/pacha-perf-agent.jar 644
deploy "$repo_root/tools/perf/pacha_phase1_remote.sh" /tmp/pacha-perf/pacha_phase1_remote.sh 755
deploy "$repo_root/tools/perf/pacha_phase1_terminal_payload.sh" /tmp/pacha-perf/pacha_phase1_terminal_payload.sh 755
if [[ $mode != minecraft ]]; then
    seq 1 200000 > "$out/seq-200000.txt"
    awk 'BEGIN { line="0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"; for (i=0;i<104858;i++) print line }' > "$out/block-8m.txt"
    deploy "$out/seq-200000.txt" /tmp/pacha-perf/seq-200000.txt 644
    deploy "$out/block-8m.txt" /tmp/pacha-perf/block-8m.txt 644
fi
"${ssh_cmd[@]}" "if test -d /tmp/pacha-perf/results; then mv /tmp/pacha-perf/results \"/tmp/pacha-perf/results.previous.\$\$\"; fi; mkdir -p /tmp/pacha-perf/results; /tmp/pacha-perf/pacha_phase1_remote.sh '$mode' 1 '$instrument'" | tee "$out/remote-driver.log"
"${ssh_cmd[@]}" 'cd /tmp/pacha-perf/results && tar -cf - .' | tar -xf - -C "$out"
rm -f "$out/seq-200000.txt" "$out/block-8m.txt"
printf 'Results: %s\n' "$out"
