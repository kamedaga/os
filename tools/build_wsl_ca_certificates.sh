#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/userland/fixtures/ca-certificates.crt"

candidates=(
  /etc/ssl/certs/ca-certificates.crt
  /etc/pki/tls/certs/ca-bundle.crt
  /etc/ssl/ca-bundle.pem
)

for src in "${candidates[@]}"; do
  if [[ -r "$src" ]]; then
    mkdir -p "$(dirname "$OUT")"
    cp "$src" "$OUT"
    exit 0
  fi
done

echo "no system CA bundle found in WSL" >&2
exit 1
