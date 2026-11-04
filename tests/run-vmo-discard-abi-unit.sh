#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
artifact_dir="$repo_dir/.artifacts/tests/vmo-discard-abi"
mkdir -p "$artifact_dir"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I"$repo_dir/userland/libpacha/include" \
    "$repo_dir/tests/vmo_discard_abi_unit.c" -o "$artifact_dir/unit"
"$artifact_dir/unit"
python3 "$repo_dir/tests/check_vmo_discard_abi.py"
python3 "$repo_dir/tests/check_ipc_register_abi.py"
