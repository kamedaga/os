#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/native-fd-growth/native_fd_growth.elf"
mkdir -p "$(dirname "$out")"
PACHAOS_MUSL_APP_SOURCE="$repo_root/tests/native_fd_growth.c" \
PACHAOS_MUSL_EXTRA_SOURCES="$repo_root/userland/libipc/src/ipc.c $repo_root/userland/libipc/src/status.c $repo_root/userland/libpacha/src/syscall.c $repo_root/userland/libpacha/src/trace.c" \
PACHAOS_MUSL_EXTRA_INCLUDE_DIRS="$repo_root/userland/libipc/include $repo_root/userland/libpacha/include" \
PACHAOS_MUSL_EXTRA_CFLAGS="-D__pachaos__ -fno-stack-protector" \
bash "$repo_root/musl/pachaos/build/build-smokes.sh" "$out"
