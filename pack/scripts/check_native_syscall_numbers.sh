#!/usr/bin/env bash
set -euo pipefail

# The checker scans every native source, so it runs as its own recipe instead
# of inside each CMake app build. The stamp is constant: passing again must not
# make the native ELFs rebuild.
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
stamp=${1:?usage: $0 <stamp>}
case "$stamp" in /*) ;; *) stamp="$repo_root/$stamp" ;; esac

python3 "$repo_root/tests/check_native_syscall_numbers.py" >/dev/null
mkdir -p "$(dirname "$stamp")"
printf 'PASS\n' > "$stamp.tmp"
mv "$stamp.tmp" "$stamp"
