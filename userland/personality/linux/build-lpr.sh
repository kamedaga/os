#!/usr/bin/env bash
set -euo pipefail

out="${1:-.artifacts/lpr-linux-x86_64.so}"
repo_root="$(cd "$(dirname "$0")/../../.." && pwd)"
mkdir -p "$(dirname "$repo_root/$out")"
zydis_source="$(bash "$repo_root/pack/scripts/download_zydis.sh")"

zydis_flags=(
  -DZYDIS_STATIC_BUILD
  -DZYDIS_MINIMAL_MODE
  -DZYDIS_DISABLE_ENCODER
  -DZYDIS_DISABLE_FORMATTER
  -DZYDIS_DISABLE_SEGMENT
  -DZYAN_NO_LIBC
)
zydis_obj_dir="$repo_root/.artifacts/lpr-zydis-v4.1.1"
mkdir -p "$zydis_obj_dir"
zydis_objects=()
for source in Decoder.c DecoderData.c SharedData.c; do
  object="$zydis_obj_dir/${source%.c}.o"
  /usr/bin/clang \
    -std=c11 -O2 -ffreestanding -fPIC -fvisibility=hidden \
    -fno-stack-protector -fno-builtin \
    "${zydis_flags[@]}" \
    -I"$zydis_source/include" \
    -I"$zydis_source/src" \
    -I"$zydis_source/dependencies/zycore/include" \
    -c "$zydis_source/src/$source" \
    -o "$object"
  zydis_objects+=("$object")
done
scanner_object="$zydis_obj_dir/lpr_runtime.o"
patch_scan_object="$zydis_obj_dir/patch_scan.o"
/usr/bin/clang \
  -std=c11 -O2 -Wall -Wextra -Werror \
  -ffreestanding -fPIC -fvisibility=hidden \
  -fno-stack-protector -fno-builtin \
  "${zydis_flags[@]}" \
  -I"$zydis_source/include" \
  -I"$zydis_source/src" \
  -I"$zydis_source/dependencies/zycore/include" \
  -I"$repo_root/userland/personality/include" \
  -I"$repo_root/userland/personality/linux/decoder" \
  -I"$repo_root/userland/libipc/include" \
  -c "$repo_root/userland/personality/linux/decoder/patch_scan.c" \
  -o "$patch_scan_object"
/usr/bin/clang \
  -std=c11 -O2 -Wall -Wextra -Werror \
  -ffreestanding -fPIC -fvisibility=hidden \
  -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer \
  -fno-stack-protector -fno-builtin \
  "${zydis_flags[@]}" \
  -I"$zydis_source/include" \
  -I"$zydis_source/src" \
  -I"$zydis_source/dependencies/zycore/include" \
  -I"$repo_root/userland/personality/include" \
  -I"$repo_root/userland/libipc/include" \
  -c "$repo_root/userland/personality/linux/runtime/lpr_runtime.c" \
  -o "$scanner_object"

/usr/bin/clang \
  -std=c11 \
  -Wall -Wextra -Werror \
  -ffreestanding \
  -fPIC \
  -fvisibility=hidden \
  -fno-omit-frame-pointer \
  -mno-omit-leaf-frame-pointer \
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
  "${zydis_flags[@]}" \
  -I"$zydis_source/include" \
  -I"$zydis_source/src" \
  -I"$zydis_source/dependencies/zycore/include" \
  -I"$repo_root/userland/personality/include" \
  -I"$repo_root/musl/pachaos/include" \
  -I"$repo_root/userland/libpacha/include" \
  -I"$repo_root/userland/libipc/include" \
  -I"$repo_root/userland/filed/include" \
  -I"$repo_root/userland/lpr_supervisor/include" \
  -I"$repo_root/userland/netd/include" \
  -I"$repo_root/userland/termd/include" \
  -I"$repo_root/userland/drmd/include" \
  -I"$repo_root/userland/inputd/include" \
  "$repo_root/userland/personality/linux/runtime/lpr_signal.c" \
  "$scanner_object" \
  "$patch_scan_object" \
  "$repo_root/userland/personality/linux/runtime/lpr_zpoline.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_syscall_catalog.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_memory.c" \
  "$repo_root/userland/personality/src/lpr_manifest.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_vfs_local.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_fd/table.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_error.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_process/client.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_filed.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_common/runtime_support.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_tty/client.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_drm/client.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_input/client.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_process/bootstrap_state.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_tty/runtime.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_fd/control.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_fd/ops.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_fd/dup_pipe.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_pipe/io.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_timerfd.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_sync_file.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_wait.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_vfs/cache.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_vfs/path.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_vfs/io.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_vfs/ops.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_process/exec.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_process/syscalls.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_fd/metadata.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_epoll.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_socket.c" \
  "$repo_root/userland/personality/linux/runtime/lpr_dispatch.c" \
  "$repo_root/userland/libipc/src/status.c" \
  "$repo_root/userland/libpacha/src/trace.c" \
  "$repo_root/userland/personality/linux/runtime/support/arena.c" \
  "$repo_root/userland/personality/linux/runtime/support/elf.c" \
  "$repo_root/userland/personality/linux/runtime/support/string.c" \
  "$repo_root/userland/personality/linux/runtime/support/syscall.c" \
  "${zydis_objects[@]}" \
  "$repo_root/userland/personality/linux/runtime/lpr_entry.S" \
  -o "$repo_root/$out"

bash "$repo_root/userland/personality/linux/check-lpr-namespace.sh" "$repo_root/$out"
