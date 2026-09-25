#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
[[ $# == 3 ]] || {
  echo "Usage: $0 core.so linux-boot-test linux-vm-client" >&2
  exit 2
}
core="$(realpath -e "$1")"
boot_test="$(realpath -e "$2")"
client="$(realpath -e "$3")"
artifact_root="$repo_root/.artifacts/tests/kobox2-linux-client-suite"
mkdir -p "$artifact_root"
out="$(mktemp -d "$artifact_root/run-XXXXXX")"
sha256sum "$core" "$boot_test" "$client" \
  "$repo_root/tests/run-kobox2-linux-client-suite.sh" >"$out/inputs.sha256"
printf 'Linux client case records: %s\n' "$out"
failures=0
for client_case in syscall fd-transfer fd-exit-race fd-inheritance fork clone thread \
    thread-exit-wait thread-exit-running thread-exit-peer client-run; do
  if timeout 60s "$boot_test" "$core" "--$client_case" "$client" \
      >"$out/$client_case.log" 2>&1 && \
      rg -q '^External syscall dispatch: status=0 .* cpus=3 warnings=0 line=0 ' \
        "$out/$client_case.log"; then
    printf 'Linux client %s PASS\n' "$client_case"
  else
    printf 'Linux client %s FAILED: %s\n' "$client_case" "$out/$client_case.log" >&2
    tail -20 "$out/$client_case.log"
    failures=$((failures + 1))
  fi
done
[[ "$failures" == 0 ]] || exit 1
printf 'Linux client suite PASS: %s\n' "$out"
