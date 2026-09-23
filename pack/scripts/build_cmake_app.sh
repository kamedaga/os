#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 3 ]; then
    echo "usage: $0 <source-dir> <build-dir> <target> [target ...]" >&2
    exit 2
fi

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
source_dir=$1
build_dir=$2
shift 2

case "$source_dir" in
    /*) ;;
    *) source_dir="$repo_root/$source_dir" ;;
esac
case "$build_dir" in
    /*) ;;
    *) build_dir="$repo_root/$build_dir" ;;
esac

# A deleted artifact tree must not erase CMake configuration.  Configure on
# every invocation; CMake itself keeps the incremental path cheap when the
# generated tree already exists.
cmake \
    -S "$source_dir" \
    -B "$build_dir" \
    -DCMAKE_BUILD_TYPE="${PACGO_CMAKE_BUILD_TYPE:-Release}" \
    -DCMAKE_C_COMPILER="${PACGO_C_COMPILER:-clang}" \
    -DCMAKE_ASM_COMPILER="${PACGO_ASM_COMPILER:-clang}"

build_args=(--build "$build_dir" --parallel "${PACGO_BUILD_JOBS:-1}" --target)
build_args+=("$@")
cmake "${build_args[@]}"
