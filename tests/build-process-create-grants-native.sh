#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests/process-create-grants"
mkdir -p "$out_dir"
PACHAOS_MUSL_APP_SOURCE="$repo_root/tests/process_create_grants_native.c" \
PACHAOS_MUSL_EXTRA_SOURCES="$repo_root/userland/libipc/src/ipc.c $repo_root/userland/libpacha/src/syscall.c" \
PACHAOS_MUSL_EXTRA_INCLUDE_DIRS="$repo_root/userland/libipc/include $repo_root/userland/libpacha/include $repo_root/userland/filed/include" \
PACHAOS_MUSL_EXTRA_CFLAGS="-D__pachaos__ -fno-stack-protector" \
bash "$repo_root/musl/pachaos/build/build-smokes.sh" "$out_dir/process_create_grants.elf"
