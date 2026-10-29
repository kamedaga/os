#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <output> <source>" >&2
    exit 2
fi

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
output=$1
source=$2

case "$output" in /*) ;; *) output="$repo_root/$output" ;; esac
case "$source" in /*) ;; *) source="$repo_root/$source" ;; esac

PACHAOS_MUSL_LIBC_DIR="${PACHAOS_MUSL_LIBC_DIR:-$repo_root/.artifacts/musl-pachaos/libc/default}" \
PACHAOS_MUSL_EXTRA_CFLAGS=-D__pachaos__ \
PACHAOS_MUSL_EXTRA_INCLUDE_DIRS="$repo_root/kobox2/protocol/include $repo_root/kobox2/protocol/generated/include${PACHAOS_MUSL_EXTRA_INCLUDE_DIRS:+ $PACHAOS_MUSL_EXTRA_INCLUDE_DIRS}" \
PACHAOS_MUSL_APP_SOURCE="$source" \
    bash "$repo_root/musl/pachaos/build/build-smokes.sh" "$output"
