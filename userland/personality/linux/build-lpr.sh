#!/usr/bin/env bash
set -euo pipefail

out="${1:-.artifacts/lpr-linux-x86_64.so}"
repo_root="$(cd "$(dirname "$0")/../../.." && pwd)"
mkdir -p "$(dirname "$repo_root/$out")"

clang \
  -std=c11 \
  -Wall -Wextra -Werror \
  -ffreestanding \
  -fPIC \
  -fvisibility=hidden \
  -fno-stack-protector \
  -fno-builtin \
  -nostdlib \
  -shared \
  -fuse-ld=lld \
  -Wl,--no-undefined \
  -Wl,-Bsymbolic \
  -Wl,-Bsymbolic-functions \
  -Wl,--version-script="$repo_root/userland/personality/linux/runtime/lpr_namespace.map" \
  -Wl,-soname,lpr-linux-x86_64.so \
  ${PACHAOS_LPR_EXTRA_CFLAGS:-} \
  -I"$repo_root/userland/personality/include" \
  -I"$repo_root/musl/pachaos/include" \
  -I"$repo_root/userland/libpacha/include" \
  -I"$repo_root/userland/libipc/include" \
  -I"$repo_root/userland/filed/include" \
  -I"$repo_root/userland/lpr_supervisor/include" \
  -I"$repo_root/userland/netd/include" \
  -I"$repo_root/userland/termd/include" \
  -I"$repo_root/userland/personality/linux/hde" \
  -DHDE64_USE_LPR_MEMSET=1 \
  "$repo_root/userland/personality/linux/runtime/lpr_runtime.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_zpoline.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_syscall_catalog.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_memory.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_vfs_local.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_fd/table.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_error.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_process/client.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_process/supervisor_fd_snapshot.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_filed.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_socket.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_dispatch.c" \
  "$repo_root/userland/libipc/src/status.c" \
  "$repo_root/userland/libpacha/src/trace.c" \
  "$repo_root/userland/personality/linux/runtime/support/arena.c" \
  "$repo_root/userland/personality/linux/runtime/support/elf.c" \
  "$repo_root/userland/personality/linux/runtime/support/string.c" \
  "$repo_root/userland/personality/linux/runtime/support/syscall.c" \
  "$repo_root/userland/personality/linux/hde/hde64.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_entry.S" \
  -o "$repo_root/$out"

bash "$repo_root/userland/personality/linux/check-lpr-namespace.sh" "$repo_root/$out"
