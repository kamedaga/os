#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Every case gets a fresh boot; retain failed runs as well as passing inputs.
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
family="${1:-}"
case "$family" in
  syscall) cases=(base rights rights-race inheritance fork clone thread
    thread-exit-wait thread-exit-running thread-exit-peer client-run) ;;
  exec) cases=(base signals vfork) ;;
  *) echo "Usage: $0 syscall|exec [--focused] [case ...]" >&2; exit 2 ;;
esac
shift
mode="--$family"
native_out="$repo_root/.artifacts/tests/kobox2-pacha-$family"
if [[ "${1:-}" == --focused ]]; then
  mode+="-focused"
  native_out+="-focused"
  shift
fi
complete_suite=1
if [[ $# -gt 0 ]]; then
  cases=("$@")
  complete_suite=0
fi
if [[ -n "${KOBOX_PACHA_MANIFEST:-}" ]]; then
  native_out+="-diagnostic"
fi
for client_case in "${cases[@]}"; do
  case "$family/$client_case" in
    syscall/base|syscall/rights|syscall/rights-race|syscall/inheritance|syscall/fork|syscall/clone|syscall/thread|syscall/thread-exit-wait|syscall/thread-exit-running|syscall/thread-exit-peer|syscall/client-run|exec/base|exec/signals|exec/vfork|exec/syscall-range) ;;
    *) echo "Unknown $family case: $client_case" >&2; exit 2 ;;
  esac
done
artifact_root="$repo_root/.artifacts/tests/kobox2-pacha-$family-suite"
mkdir -p "$artifact_root"
run_dir="$(mktemp -d "$artifact_root/run-XXXXXX")"
printf 'Client case records: %s\n' "$run_dir"
failures=0
for client_case in "${cases[@]}"; do
  case_dir="$run_dir/$client_case"
  mkdir -p "$case_dir"
  printf 'family=%s\ncase=%s\nmode=%s\nexec_entry=call\n' \
    "$family" "$client_case" "$mode" \
    >"$case_dir/settings.txt"
  sha256sum "$repo_root"/userland/kobox2_adapter/*.[chS] \
    "$repo_root"/tests/kobox2_exec*.[cS] \
    "$repo_root/kobox2/linux-sandbox/kobox/tests/clients/vm_program.c" \
    "$repo_root/kobox2/linux-sandbox/kobox/tests/clients/vm_program.h" \
    "$repo_root/kobox2/linux-sandbox/kobox/arch/x86_64/vm_program.S" \
    "$repo_root/userland/kobox2_adapter/vm_client.ld" \
    "$repo_root/userland/personality/linux/runtime/lpr_syscall_entry.inc" \
    "$repo_root"/tests/build-kobox2-pacha-*.sh \
    "$repo_root/tests/run-kobox2-pacha-foundation.sh" \
    "$repo_root/tests/run-kobox2-pacha-client-suite.sh" >"$case_dir/sources.sha256"
  if [[ -n "${KOBOX_PACHA_MANIFEST:-}" ]]; then
    sha256sum "$KOBOX_PACHA_MANIFEST" >>"$case_dir/sources.sha256"
  fi
  result=0
  KOBOX_PACHA_EXEC_CASE="$client_case" KOBOX_PACHA_SYSCALL_CASE="$client_case" \
    bash "$repo_root/tests/run-kobox2-pacha-foundation.sh" "$mode" \
    >"$case_dir/run.log" 2>&1 || result=$?
  for artifact in serial.log qemu.log inputs.sha256; do
    if [[ -f "$native_out/$artifact" && "$native_out/$artifact" -nt "$case_dir/sources.sha256" ]]; then
      cp "$native_out/$artifact" "$case_dir/$artifact"
    fi
  done
  if [[ "$result" -ne 0 ]]; then
    printf '%s case %s FAILED: %s\n' "$family" "$client_case" "$case_dir/run.log" >&2
    tail -n 30 "$case_dir/run.log"
    failures=$((failures + 1))
  else
    printf '%s case %s PASS\n' "$family" "$client_case"
  fi
done
if [[ "$failures" -ne 0 ]]; then
  printf '%s cases failed: %s; records: %s\n' "$family" "$failures" "$run_dir" >&2
  exit 1
fi
if [[ "$complete_suite" == 1 ]]; then
  printf 'PachaOS %s suite PASS (%s): %s\n' "$family" "$mode" "$run_dir"
else
  printf 'Selected %s cases passed; full suite was not requested.\n' "$family"
fi
