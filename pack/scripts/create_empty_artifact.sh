#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <artifact-path>" >&2
    exit 2
fi

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
case "$1" in
    .artifacts/*) output="$repo_root/$1" ;;
    *) echo "output must be below .artifacts: $1" >&2; exit 2 ;;
esac
mkdir -p "$(dirname -- "$output")"
: >"$output"
