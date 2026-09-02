#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

runs=${XFCE_STABILITY_RUNS:-8}
ready_timeout=${XFCE_STABILITY_READY_TIMEOUT_SECONDS:-180}
diagnostic_timeout=${XFCE_STABILITY_DIAGNOSTIC_TIMEOUT_SECONDS:-15}
qemu_timeout=${XFCE_STABILITY_QEMU_TIMEOUT_SECONDS:-225}
cpus=${XFCE_STABILITY_CPUS:-4}
run_id="$(date -u +%Y%m%dT%H%M%SZ)-$$"
result_root=${XFCE_STABILITY_RESULT_ROOT:-"${repo_root}/.artifacts/test-results/xfce-startup/stability-${run_id}"}

for value_name in runs ready_timeout diagnostic_timeout qemu_timeout cpus; do
  value=${!value_name}
  [[ "${value}" =~ ^[1-9][0-9]*$ ]] || {
    printf '%s must be a positive integer: %s\n' "${value_name}" "${value}" >&2
    exit 2
  }
done

mkdir -p "$(dirname "${result_root}")"
if ! mkdir "${result_root}"; then
  printf 'refusing to overwrite stability result directory: %s\n' \
    "${result_root}" >&2
  exit 2
fi
summary="${result_root}/summary.tsv"
printf 'run\tclassification\tevidence_complete\tcpu_activity\tdetected_ms\tpacgo_status\n' \
  >"${summary}"

if [[ ${SKIP_SYNC:-0} != 1 ]]; then
  PACGO_PROGRESS=plain .artifacts/bin/pacgo sync rootfs --force
  PACGO_PROGRESS=plain .artifacts/bin/pacgo sync bootfs
fi

ready_count=0
anomaly_count=0
harness_error_count=0

for ((run = 1; run <= runs; ++run)); do
  trial="${result_root}/run-$(printf '%03d' "${run}")"
  mkdir "${trial}"
  qmp_socket="${trial}/qmp.sock"
  printf 'XFCE_STABILITY_RUN_START run=%s/%s dir=%s\n' \
    "${run}" "${runs}" "${trial}"

  set +e
  XFCE_STARTUP_QMP_SOCKET="${qmp_socket}" \
  XFCE_STARTUP_READY_TIMEOUT="${ready_timeout}" \
  XFCE_STARTUP_DIAGNOSTIC_TIMEOUT="${diagnostic_timeout}" \
  XFCE_STARTUP_CPUS="${cpus}" \
  XFCE_STARTUP_RESULT="${trial}/result.json" \
  XFCE_STARTUP_CPU_INFO="${trial}/qmp-info-cpus.txt" \
  XFCE_STARTUP_PROCESS_TREE="${trial}/process-tree.txt" \
  XFCE_STARTUP_SCREENSHOT="${trial}/screenshot.png" \
    .artifacts/bin/pacgo qemu-test \
      --console-shell \
      --cpus "${cpus}" \
      --timeout "${qemu_timeout}s" \
      --graphics 2d \
      --input-profile keyboard-tablet \
      --qemu-arg=-qmp \
      --qemu-arg="unix:${qmp_socket},server=on,wait=off" \
      --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
      --python tests/qemu_xfce_startup_stability.py
  pacgo_status=$?
  set -e

  for source in \
    .artifacts/console-tty-test.log \
    .artifacts/serial-tty-test.log \
    .artifacts/qemu-tty-host-time.log \
    .artifacts/qemu-tty-python.log \
    .artifacts/qemu-limine.log \
    .artifacts/qemu.log; do
    [[ ! -f "${source}" ]] || cp "${source}" "${trial}/${source##*/}"
  done

  classification=harness-error
  evidence_complete=false
  cpu_activity=
  detected_ms=
  if [[ -s "${trial}/result.json" ]]; then
    classification="$({
      sed -n 's/.*"classification": "\([^"]*\)".*/\1/p' \
        "${trial}/result.json"
    } | tail -n 1)"
    evidence_complete="$({
      sed -n 's/.*"evidence_complete": \([^,}]*\).*/\1/p' \
        "${trial}/result.json"
    } | tail -n 1)"
    cpu_activity="$({
      sed -n 's/.*"cpu_activity": "\([^"]*\)".*/\1/p' \
        "${trial}/result.json"
    } | tail -n 1)"
    detected_ms="$({
      sed -n 's/.*"detected_ms": \([^,}]*\).*/\1/p' "${trial}/result.json"
    } | tail -n 1)"
  fi
  [[ -n "${classification}" ]] || classification=harness-error

  if [[ "${classification}" == ready && "${pacgo_status}" == 0 ]]; then
    ready_count=$((ready_count + 1))
  elif [[ "${classification}" == hang || \
          "${classification}" == session-exit || \
          "${classification}" == console-closed ]]; then
    anomaly_count=$((anomaly_count + 1))
    if [[ "${evidence_complete}" != true ]]; then
      harness_error_count=$((harness_error_count + 1))
    fi
  else
    harness_error_count=$((harness_error_count + 1))
  fi

  printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
    "${run}" "${classification}" "${evidence_complete}" \
    "${cpu_activity}" "${detected_ms}" "${pacgo_status}" >>"${summary}"
  printf 'XFCE_STABILITY_RUN_DONE run=%s/%s classification=%s evidence=%s cpu_activity=%s dir=%s\n' \
    "${run}" "${runs}" "${classification}" "${evidence_complete}" \
    "${cpu_activity:-n/a}" "${trial}"
done

printf 'XFCE_STABILITY_SUMMARY runs=%s ready=%s anomalies=%s harness_errors=%s root=%s\n' \
  "${runs}" "${ready_count}" "${anomaly_count}" "${harness_error_count}" \
  "${result_root}"

if ((anomaly_count != 0 || harness_error_count != 0)); then
  exit 1
fi
