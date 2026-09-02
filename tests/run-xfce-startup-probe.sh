#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

run="${XFCE_IDLE_RUN:-baseline}"
wait_seconds="${XFCE_IDLE_WAIT_SECONDS:-75}"
timeout_seconds="${XFCE_IDLE_TIMEOUT_SECONDS:-105}"
out_dir="${repo_root}/.artifacts/test-results/xfce-startup/${run}"
qmp_socket="${out_dir}/qmp.sock"
mkdir -p "${out_dir}"
rm -f "${qmp_socket}" "${out_dir}/result.json" "${out_dir}/desktop.png"

XFCE_IDLE_QMP_SOCKET="${qmp_socket}" \
XFCE_IDLE_WAIT_SECONDS="${wait_seconds}" \
XFCE_IDLE_SCREENSHOT="${out_dir}/desktop.png" \
XFCE_IDLE_SCREENSHOT_FORMAT=png \
XFCE_IDLE_RESULT="${out_dir}/result.json" \
XFCE_IDLE_CPU_DUMP="${out_dir}/cpu-state.txt" \
  .artifacts/bin/pacgo qemu-test \
    --cpus 4 \
    --timeout "${timeout_seconds}s" \
    --qemu-arg=-qmp \
    --qemu-arg="unix:${qmp_socket},server=on,wait=off" \
    --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
    --python tests/qemu_xfce_idle_probe.py

for source in \
  .artifacts/console-tty-test.log \
  .artifacts/serial-tty-test.log \
  .artifacts/qemu-tty-host-time.log \
  .artifacts/qemu-tty-python.log; do
  [[ ! -f "${source}" ]] || cp "${source}" "${out_dir}/${source##*/}"
done

cat "${out_dir}/result.json"
