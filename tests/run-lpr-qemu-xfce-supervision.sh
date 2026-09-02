#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

runs=${XFCE_SUPERVISION_RUNS:-8}
probe_timeout=${XFCE_SUPERVISION_TIMEOUT_SECONDS:-240}
diagnostic_timeout=${XFCE_SUPERVISION_DIAGNOSTIC_TIMEOUT_SECONDS:-15}
qemu_timeout=${XFCE_SUPERVISION_QEMU_TIMEOUT_SECONDS:-285}
cpus=${XFCE_SUPERVISION_CPUS:-4}
run_id="$(date -u +%Y%m%dT%H%M%SZ)-$$"
result_root=${XFCE_SUPERVISION_RESULT_ROOT:-"${repo_root}/.artifacts/test-results/xfce-supervision/run-${run_id}"}

for value_name in runs probe_timeout diagnostic_timeout qemu_timeout cpus; do
  value=${!value_name}
  [[ "${value}" =~ ^[1-9][0-9]*$ ]] || {
    printf '%s must be a positive integer: %s\n' "${value_name}" "${value}" >&2
    exit 2
  }
done

mkdir -p "$(dirname "${result_root}")"
if ! mkdir "${result_root}"; then
  printf 'refusing to overwrite supervision result directory: %s\n' \
    "${result_root}" >&2
  exit 2
fi

summary="${result_root}/summary.tsv"
printf 'run\tclassification\tevidence_complete\thlt_count\tdetected_ms\tpacgo_status\n' \
  >"${summary}"

if [[ ${SKIP_SYNC:-0} != 1 ]]; then
  PACGO_PROGRESS=plain .artifacts/bin/pacgo sync rootfs --force
  PACGO_PROGRESS=plain .artifacts/bin/pacgo sync bootfs
fi

pass_count=0
failure_count=0
harness_error_count=0

for ((run = 1; run <= runs; ++run)); do
  trial="${result_root}/run-$(printf '%03d' "${run}")"
  mkdir "${trial}"
  qmp_socket="${trial}/qmp.sock"
  printf 'XFCE_SUPERVISION_RUN_START run=%s/%s dir=%s\n' \
    "${run}" "${runs}" "${trial}"

  set +e
  XFCE_SUPERVISION_QMP_SOCKET="${qmp_socket}" \
  XFCE_SUPERVISION_TIMEOUT="${probe_timeout}" \
  XFCE_SUPERVISION_DIAGNOSTIC_TIMEOUT="${diagnostic_timeout}" \
  XFCE_SUPERVISION_CPUS="${cpus}" \
  XFCE_SUPERVISION_RESULT="${trial}/result.json" \
  XFCE_SUPERVISION_CPU_DUMP="${trial}/cpu-state.txt" \
  XFCE_SUPERVISION_PROCESS_TREE="${trial}/process-tree.txt" \
  XFCE_SUPERVISION_SCREENSHOT="${trial}/screenshot.png" \
    .artifacts/bin/pacgo qemu-test \
      --console-shell \
      --cpus "${cpus}" \
      --timeout "${qemu_timeout}s" \
      --graphics 2d \
      --input-profile keyboard-tablet \
      --qemu-arg=-qmp \
      --qemu-arg="unix:${qmp_socket},server=on,wait=off" \
      --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
      --python tests/qemu_xfce_supervision.py
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
  hlt_count=
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
    hlt_count="$({
      sed -n 's/.*"hlt_count": \([^,}]*\).*/\1/p' \
        "${trial}/result.json"
    } | tail -n 1)"
    detected_ms="$({
      sed -n 's/.*"detected_ms": \([^,}]*\).*/\1/p' \
        "${trial}/result.json"
    } | tail -n 1)"
  fi
  [[ -n "${classification}" ]] || classification=harness-error

  # The Python probe watches this live.  Recheck the preserved copy so a
  # marker emitted during QEMU shutdown cannot turn a faulty run into a pass.
  if [[ -s "${trial}/serial-tty-test.log" ]] &&
      rg -q 'GENERAL PROTECTION|INVALID OPCODE|PAGE FAULT|USER fault|DOUBLE FAULT|KERNEL PANIC' \
        "${trial}/serial-tty-test.log"; then
    classification=kernel-fault
  fi

  if [[ "${classification}" == pass && "${pacgo_status}" == 0 ]]; then
    pass_count=$((pass_count + 1))
  elif [[ "${classification}" == harness-error || \
          "${evidence_complete}" != true ]]; then
    failure_count=$((failure_count + 1))
    harness_error_count=$((harness_error_count + 1))
  else
    failure_count=$((failure_count + 1))
  fi

  printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
    "${run}" "${classification}" "${evidence_complete}" \
    "${hlt_count}" "${detected_ms}" "${pacgo_status}" >>"${summary}"
  printf 'XFCE_SUPERVISION_RUN_DONE run=%s/%s classification=%s evidence=%s hlt=%s dir=%s\n' \
    "${run}" "${runs}" "${classification}" "${evidence_complete}" \
    "${hlt_count:-n/a}" "${trial}"
done

failure_milli_rate=$((failure_count * 100000 / runs))
printf 'XFCE_SUPERVISION_SUMMARY runs=%s pass=%s failures=%s failure_rate=%s.%03s%% harness_errors=%s root=%s\n' \
  "${runs}" "${pass_count}" "${failure_count}" \
  "$((failure_milli_rate / 1000))" "$(printf '%03d' "$((failure_milli_rate % 1000))")" \
  "${harness_error_count}" "${result_root}" | tee "${result_root}/summary.txt"

if ((failure_count != 0 || harness_error_count != 0)); then
  exit 1
fi
