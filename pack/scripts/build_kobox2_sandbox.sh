#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Builds one native kobox2 sandbox and rejects it if it defines a symbol owned
# by another device service. Usage:
#   build_kobox2_sandbox.sh OUT FORBIDDEN_SYMBOL_REGEX BUILD_SANDBOX_FLAG...
# The sandbox is userland-only. Keeping it out of the Linux package recipes
# means a libpacha or adapter edit relinks this ELF, not the kbuild runtime.
set -euo pipefail

if [[ $# -lt 3 ]]; then
    echo "usage: $0 OUT FORBIDDEN_SYMBOL_REGEX BUILD_SANDBOX_FLAG..." >&2
    exit 2
fi
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
out="$1"
forbidden="$2"
shift 2
case "$out" in /*) ;; *) out="$repo_root/$out" ;; esac

mkdir -p "$(dirname "$out")"
bash "$repo_root/userland/kobox2_adapter/build-sandbox.sh" "$out" "$@"
if nm --defined-only "$out" | rg " [TtDdBb] ($forbidden)"; then
    echo "Unrelated device service code leaked into ${out#"$repo_root/"}" >&2
    exit 1
fi
