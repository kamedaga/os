#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Fresh native processes and a fresh upstream boot for every common VM case.
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
artifact_root="$repo_root/.artifacts/tests/kobox2-pacha-vm-suite"
mkdir -p "$artifact_root"
run_dir="$(mktemp -d "$artifact_root/run-XXXXXX")"
cases=(basic readonly reuse irq truncate late-fault pressure-fault
  exit-publish exit lifetime death rollback)
complete_suite=1
keep_going=0
failures=0
mode=--vm
native_out="$repo_root/.artifacts/tests/kobox2-pacha-vm"
if [[ "${1:-}" == --focused ]]; then
  mode=--vm-focused
  native_out+="-focused"
  shift
fi
if [[ "${1:-}" == --keep-going ]]; then
  keep_going=1
  shift
fi
if [[ -n "${KOBOX_PACHA_MANIFEST:-}" ]]; then
  native_out+="-diagnostic"
fi
if [[ $# -gt 0 ]]; then
  cases=("$@")
  complete_suite=0
fi
printf 'VM case records: %s\n' "$run_dir"
for vm_case in "${cases[@]}"; do
  # Keep caller-provided names out of paths until they match a known case.
  case "$vm_case" in
    basic|readonly|reuse|irq|truncate|late-fault|pressure-fault|exit-publish|exit|lifetime|death|rollback) ;;
    *) printf 'Unknown VM case: %s\n' "$vm_case" >&2; exit 2 ;;
  esac
  case_dir="$run_dir/$vm_case"
  mkdir -p "$case_dir"
  sha256sum "$repo_root"/userland/kobox2_adapter/*.[chS] \
    "$repo_root/userland/kobox2_adapter/vm_client.ld" \
    "$repo_root/userland/personality/linux/runtime/lpr_syscall_entry.inc" \
    "$repo_root/kobox2/linux-sandbox/kobox/tests/clients/vm_program.c" \
    "$repo_root/kobox2/linux-sandbox/kobox/tests/clients/vm_program.h" \
    "$repo_root/kobox2/linux-sandbox/kobox/arch/x86_64/vm_program.S" \
    "$repo_root/tests/build-kobox2-pacha-foundation.sh" \
    "$repo_root/tests/build-kobox2-pacha-vm-client.sh" \
    "$repo_root/tests/run-kobox2-pacha-foundation.sh" \
    "$repo_root/tests/run-kobox2-pacha-vm-suite.sh" >"$case_dir/sources.sha256"
  if [[ -n "${KOBOX_PACHA_MANIFEST:-}" ]]; then
    sha256sum "$KOBOX_PACHA_MANIFEST" >>"$case_dir/sources.sha256"
  fi
  result=0
  KOBOX_PACHA_VM_CASE="$vm_case" KOBOX_PACHA_TIMEOUT="${KOBOX_PACHA_TIMEOUT:-300}" \
    bash "$repo_root/tests/run-kobox2-pacha-foundation.sh" "$mode" \
    >"$case_dir/run.log" 2>&1 || result=$?
  if ! sha256sum -c "$case_dir/sources.sha256" >"$case_dir/source-check.log" 2>&1; then
    printf 'VM case %s sources changed during execution\n' "$vm_case" >&2
    result=1
  fi
  for artifact in serial.log qemu.log inputs.sha256; do
    # A failed build must not attach an earlier boot's log or input hashes.
    if [[ -f "$native_out/$artifact" && "$native_out/$artifact" -nt "$case_dir/sources.sha256" ]]; then
      cp "$native_out/$artifact" "$case_dir/$artifact"
    fi
  done
  if [[ "$result" -ne 0 ]]; then
    printf 'VM case %s FAILED: %s\n' "$vm_case" "$case_dir/run.log" >&2
    tail -n 45 "$case_dir/run.log"
    failures=$((failures + 1))
    if [[ "$keep_going" == 0 ]]; then
      exit "$result"
    fi
    continue
  fi
  printf 'VM case %s PASS\n' "$vm_case"
done
if [[ "$failures" -ne 0 ]]; then
  printf 'VM cases failed: %s; records: %s\n' "$failures" "$run_dir" >&2
  exit 1
fi
if [[ "$complete_suite" == 1 ]]; then
  if [[ "$mode" == --vm-focused ]]; then
    printf 'PACHA_KOBOX_VM_FOCUSED_SUITE=PASS\n'
  else
    printf 'PACHA_KOBOX_VM_SUITE=PASS\n'
  fi
else
  printf 'Selected VM cases passed; full suite was not requested.\n'
fi
