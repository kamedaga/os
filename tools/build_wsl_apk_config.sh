#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$root_dir/.artifacts/userland-fixtures"

mkdir -p "$out_dir"

printf '%s\n' "x86_64" > "$out_dir/apk-arch"
cat > "$out_dir/apk-repositories" <<'EOF'
http://dl-cdn.alpinelinux.org/alpine/v3.22/main
http://dl-cdn.alpinelinux.org/alpine/v3.22/community
EOF
: > "$out_dir/apk-world"
