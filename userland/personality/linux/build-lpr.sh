#!/usr/bin/env bash
set -euo pipefail

# Builds the LPR runtime with the CMake project next to this script (Ninja
# recompiles only changed translation units) and installs it at OUT. It needs
# the musl_libc and zydis_source pacgo recipes' outputs.
out="${1:-.artifacts/lpr-linux-x86_64.so}"
repo_root="$(cd "$(dirname "$0")/../../.." && pwd)"
case "$out" in /*) ;; *) out="$repo_root/$out" ;; esac
build_dir="$repo_root/.artifacts/cmake/lpr"

bash "$repo_root/pack/scripts/build_cmake_app.sh" \
  "$repo_root/userland/personality/linux/CMakeLists.txt" "$build_dir" lpr_runtime

# An unchanged runtime keeps OUT's timestamp, so its consumers stay current.
built="$build_dir/lpr-linux-x86_64.so"
if ! cmp -s "$built" "$out"; then
  mkdir -p "$(dirname "$out")"
  cp "$built" "$out.tmp"
  mv "$out.tmp" "$out"
fi
