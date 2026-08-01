#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

timeout=${P4_STEP5_TIMEOUT:-180}
out_dir=.artifacts/phase4-step5
sample_log=$out_dir/4cpu-vcpu-samples.txt
mkdir -p "$out_dir"

stop_qemu()
{
    pkill -TERM qemu-system-x86 2>/dev/null || true
}
trap stop_qemu EXIT
stop_qemu
rm -f .artifacts/console-tty-test.log .artifacts/serial-tty-test.log \
    .artifacts/qemu-tty-vcpus.tsv

if [[ ${SKIP_SYNC:-0} != 1 ]]; then
    .artifacts/bin/pacgo sync rootfs
    .artifacts/bin/pacgo sync bootfs
fi

.artifacts/bin/pacgo qemu-test \
    --cpus 4 \
    --timeout "${timeout}s" \
    --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
    --send 'P3A_INPUT_ONLY=1 P3A_INPUT_MODE=mouse bash /cmd/phase4_gui_benchmark.sh' \
    --expect 'P4_BENCH_FRAME source=' \
    --expect 'P3A_INPUT_PASS mode=mouse direct_sway=1 classification=1' \
    --input-send-event \
      'P4_BENCH_INPUT_READY source=wayland-event-time@key:a=down,key:a=up,rel:x=7,rel:y=-4,btn:left=down,btn:left=up' &
runner_pid=$!

console=.artifacts/console-tty-test.log
marker_deadline=$((SECONDS + timeout))
while (( SECONDS < marker_deadline )); do
    if [[ -f $console ]] && rg -q '^P4_BENCH_INPUT count=' "$console"; then
        break
    fi
    kill -0 "$runner_pid" 2>/dev/null || break
    sleep 0.1
done
if [[ ! -f $console ]] || ! rg -q '^P4_BENCH_INPUT count=' "$console"; then
    wait "$runner_pid" || true
    printf 'Step 5 benchmark did not reach the animation window\n' >&2
    exit 1
fi

qemu_pid=$(pgrep -n -x qemu-system-x86 || true)
if [[ -z $qemu_pid ]]; then
    wait "$runner_pid" || true
    printf 'Step 5 benchmark could not find the QEMU process\n' >&2
    exit 1
fi

declare -A previous_ticks=()
declare -A total_ticks=()
declare -A cpu_seen=()
declare -A vcpu_for_tid=()
while IFS=$'\t' read -r cpu tid; do
    [[ -n $cpu && -n $tid ]] || continue
    vcpu_for_tid[$tid]=$cpu
done <.artifacts/qemu-tty-vcpus.tsv
if (( ${#vcpu_for_tid[@]} != 4 )); then
    wait "$runner_pid" || true
    printf 'Step 5 benchmark expected 4 QMP vCPU thread IDs, got %d\n' \
        "${#vcpu_for_tid[@]}" >&2
    exit 1
fi
max_parallel=0
sample_count=0
: >"$sample_log"
printf 'qemu_pid=%s cmdline=' "$qemu_pid" >>"$sample_log"
tr '\0' ' ' <"/proc/$qemu_pid/cmdline" >>"$sample_log"
printf '\nvcpu_threads=' >>"$sample_log"
for tid in "${!vcpu_for_tid[@]}"; do
    printf '%s:%s,' "${vcpu_for_tid[$tid]}" "$tid" >>"$sample_log"
done
printf '\n' >>"$sample_log"
while kill -0 "$runner_pid" 2>/dev/null; do
    if rg -q '^P4_BENCH_ANIMATION_DONE ' "$console"; then
        break
    fi
    declare -A interval_active=()
    for tid in "${!vcpu_for_tid[@]}"; do
        task_dir=/proc/$qemu_pid/task/$tid
        [[ -r $task_dir/stat ]] || continue
        IFS= read -r stat_line <"$task_dir/stat" || continue
        stat_tail=${stat_line##*) }
        read -r -a fields <<<"$stat_tail"
        (( ${#fields[@]} >= 13 )) || continue
        ticks=$((fields[11] + fields[12]))
        if [[ -n ${previous_ticks[$tid]+x} ]]; then
            delta=$((ticks - previous_ticks[$tid]))
            if (( delta > 0 )); then
                cpu=${vcpu_for_tid[$tid]}
                interval_active[$cpu]=1
                cpu_seen[$cpu]=1
                total_ticks[$cpu]=$(( ${total_ticks[$cpu]:-0} + delta ))
            fi
        fi
        previous_ticks[$tid]=$ticks
    done
    parallel=${#interval_active[@]}
    (( parallel > max_parallel )) && max_parallel=$parallel
    printf 'sample=%d active_vcpus=%d\n' "$sample_count" "$parallel" >>"$sample_log"
    sample_count=$((sample_count + 1))
    sleep 0.05
done

if ! wait "$runner_pid"; then
    printf 'Step 5 QEMU scenario failed\n' >&2
    exit 1
fi
cp .artifacts/console-tty-test.log "$out_dir/console.log"
cp .artifacts/serial-tty-test.log "$out_dir/serial.log"

if (( max_parallel < 2 )); then
    printf 'Step 5 scenario observed only %d concurrently progressing vCPU(s)\n' "$max_parallel" >&2
    exit 1
fi

{
    printf 'P4_STEP5_SMP samples=%d max_parallel_vcpus=%d union_vcpus=%d' \
        "$sample_count" "$max_parallel" "${#cpu_seen[@]}"
    for cpu in "${!total_ticks[@]}"; do
        printf ' cpu%s_ticks=%d' "$cpu" "${total_ticks[$cpu]}"
    done
    printf '\n'
} | tee "$out_dir/summary.txt"
