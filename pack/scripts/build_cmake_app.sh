#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 3 ]; then
    echo "usage: $0 <source-dir>/CMakeLists.txt <build-dir> <target> [target ...]" >&2
    exit 2
fi

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
source "$repo_root/pack/scripts/lib/jobs.sh"
source_dir=$1
build_dir=$2
shift 2

case "$source_dir" in
    /*) ;;
    *) source_dir="$repo_root/$source_dir" ;;
esac
# pacgo declares every existing path in a recipe's argv as an input, so the
# recipes name the project file: a whole source directory would make tests and
# documentation inputs too. Compiled files come from Ninja discovery instead.
if [[ "${source_dir##*/}" == CMakeLists.txt ]]; then
    source_dir=${source_dir%/*}
fi
case "$build_dir" in
    /*) ;;
    *) build_dir="$repo_root/$build_dir" ;;
esac

toolchain="$repo_root/userland/cmake/pachaos-native.toolchain.cmake"

# A deleted artifact tree must not erase CMake configuration.  Configure on
# every invocation; CMake itself keeps the incremental path cheap when the
# generated tree already exists.  A tree configured with another generator or
# toolchain keeps compiler checks from that setup, so it is configured afresh.
# The tools CMake would otherwise cache at the first configure are the ones on
# PATH now, the same files the recipe's tool state records.
cmake_fresh=()
if [[ -f "$build_dir/CMakeCache.txt" ]] &&
   { [[ "$(_pacgo_cmake_cache_value "$build_dir" CMAKE_GENERATOR)" != Ninja ]] ||
     [[ "$(_pacgo_cmake_cache_value "$build_dir" CMAKE_TOOLCHAIN_FILE)" != "$toolchain" ]]; }; then
    cmake_fresh=(--fresh)
fi
cmake \
    "${cmake_fresh[@]}" \
    -S "$source_dir" \
    -B "$build_dir" \
    -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
    -DCMAKE_BUILD_TYPE="${PACGO_CMAKE_BUILD_TYPE:-Release}" \
    -DCMAKE_C_COMPILER="${PACGO_C_COMPILER:-${CAPOS_FREESTANDING_CC:-clang}}" \
    -DCMAKE_ASM_COMPILER="${PACGO_ASM_COMPILER:-${CAPOS_FREESTANDING_CC:-clang}}" \
    -DCMAKE_MAKE_PROGRAM="$(command -v ninja)" \
    -DCMAKE_AR="$(command -v ar)" \
    -DCMAKE_RANLIB="$(command -v ranlib)"

build_args=("$build_dir" --target)
build_args+=("$@")
pacgo_cmake_build "${PACGO_BUILD_JOBS:-1}" "${build_args[@]}"
