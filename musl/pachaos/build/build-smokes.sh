#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../../.." && pwd)"
out_dir="$repo_root/.artifacts/musl-pachaos"
obj_dir="$out_dir/obj"
sysroot="$out_dir/sysroot"
cc="${CAPOS_FREESTANDING_CC:-clang}"
ar="${CAPOS_AR:-ar}"
target="${PACHAOS_MUSL_DRIVER_TARGET:-x86_64-linux-musl}"
upstream="$repo_root/musl/upstream"

rm -rf "$obj_dir" "$sysroot"
mkdir -p \
  "$out_dir" \
  "$obj_dir/include/bits" \
  "$obj_dir/src/internal" \
  "$obj_dir/libc" \
  "$obj_dir/app" \
  "$sysroot/usr/include" \
  "$sysroot/usr/lib"

sed -f "$upstream/tools/mkalltypes.sed" \
  "$upstream/arch/pachaos/bits/alltypes.h.in" \
  "$upstream/include/alltypes.h.in" \
  > "$obj_dir/include/bits/alltypes.h"

cp "$upstream/arch/pachaos/bits/syscall.h.in" "$obj_dir/include/bits/syscall.h"
sed -n -e 's/__NR_/SYS_/p' "$upstream/arch/pachaos/bits/syscall.h.in" >> "$obj_dir/include/bits/syscall.h"
printf '#define VERSION "%s"\n' "$(cat "$upstream/VERSION")" > "$obj_dir/src/internal/version.h"

cp -R "$upstream/include/." "$sysroot/usr/include/"
mkdir -p "$sysroot/usr/include/bits" "$sysroot/usr/include/pachaos"
cp -R "$upstream/arch/generic/bits/." "$sysroot/usr/include/bits/"
cp -R "$upstream/arch/pachaos/bits/." "$sysroot/usr/include/bits/"
cp "$obj_dir/include/bits/alltypes.h" "$sysroot/usr/include/bits/alltypes.h"
cp "$obj_dir/include/bits/syscall.h" "$sysroot/usr/include/bits/syscall.h"
cp "$repo_root/musl/pachaos/include/pachaos/abi.h" "$sysroot/usr/include/pachaos/abi.h"

common_cflags=(
  -target "$target"
  --sysroot "$sysroot"
  -std=c99
  -fno-stack-protector
  -fno-plt
  -fPIE
  -mno-red-zone
  -O2
  -Wall
  -Wextra
  -D_XOPEN_SOURCE=700
  -I "$upstream/arch/pachaos"
  -I "$upstream/arch/generic"
  -I "$obj_dir/src/internal"
  -I "$upstream/src/include"
  -I "$upstream/src/internal"
  -I "$obj_dir/include"
)

libc_sources=(
  "$upstream/src/env/__libc_start_main.c"
  "$upstream/src/internal/libc.c"
  "$upstream/src/internal/defsysinfo.c"
  "$upstream/src/internal/procfdname.c"
  "$upstream/src/env/__environ.c"
  "$upstream/src/env/__init_tls.c"
  "$upstream/src/thread/default_attr.c"
  "$upstream/src/thread/__syscall_cp.c"
  "$upstream/src/internal/syscall_ret.c"
  "$upstream/src/errno/__errno_location.c"
  "$upstream/src/errno/strerror.c"
  "$upstream/src/exit/exit.c"
  "$upstream/src/exit/_Exit.c"
  "$upstream/src/unistd/read.c"
  "$upstream/src/unistd/write.c"
  "$upstream/src/unistd/close.c"
  "$upstream/src/unistd/readv.c"
  "$upstream/src/unistd/writev.c"
  "$upstream/src/unistd/isatty.c"
  "$upstream/src/select/poll.c"
  "$upstream/src/misc/ioctl.c"
  "$upstream/src/stat/fstat.c"
  "$upstream/src/stat/fstatat.c"
  "$upstream/src/mman/mmap.c"
  "$upstream/src/mman/munmap.c"
  "$upstream/src/mman/mprotect.c"
  "$upstream/src/mman/madvise.c"
  "$upstream/src/mman/mremap.c"
  "$upstream/src/misc/syscall.c"
  "$upstream/src/malloc/lite_malloc.c"
  "$upstream/src/malloc/mallocng/malloc.c"
  "$upstream/src/malloc/mallocng/free.c"
  "$upstream/src/malloc/mallocng/realloc.c"
  "$upstream/src/malloc/mallocng/aligned_alloc.c"
  "$upstream/src/malloc/mallocng/malloc_usable_size.c"
  "$upstream/src/malloc/mallocng/donate.c"
  "$upstream/src/malloc/calloc.c"
  "$upstream/src/malloc/free.c"
  "$upstream/src/malloc/realloc.c"
  "$upstream/src/malloc/reallocarray.c"
  "$upstream/src/malloc/replaced.c"
  "$upstream/src/time/clock_gettime.c"
  "$upstream/src/thread/__lock.c"
  "$upstream/src/unistd/lseek.c"
  "$upstream/src/stdio/__lockfile.c"
  "$upstream/src/stdio/__overflow.c"
  "$upstream/src/stdio/__stdio_close.c"
  "$upstream/src/stdio/__stdio_exit.c"
  "$upstream/src/stdio/__stdio_read.c"
  "$upstream/src/stdio/__stdio_seek.c"
  "$upstream/src/stdio/__stdio_write.c"
  "$upstream/src/stdio/__stdout_write.c"
  "$upstream/src/stdio/__towrite.c"
  "$upstream/src/stdio/fflush.c"
  "$upstream/src/stdio/fprintf.c"
  "$upstream/src/stdio/fwrite.c"
  "$upstream/src/stdio/ofl.c"
  "$upstream/src/stdio/printf.c"
  "$upstream/src/stdio/snprintf.c"
  "$upstream/src/stdio/stderr.c"
  "$upstream/src/stdio/stdout.c"
  "$upstream/src/stdio/vfprintf.c"
  "$upstream/src/stdio/vsnprintf.c"
  "$upstream/src/locale/__lctrans.c"
  "$upstream/src/multibyte/internal.c"
  "$upstream/src/multibyte/wcrtomb.c"
  "$upstream/src/multibyte/wctomb.c"
  "$upstream/src/math/__fpclassifyl.c"
  "$upstream/src/math/__signbitl.c"
  "$upstream/src/math/frexpl.c"
  "$upstream/src/math/scalbn.c"
  "$upstream/src/string/memcpy.c"
  "$upstream/src/string/memchr.c"
  "$upstream/src/string/strlen.c"
  "$upstream/src/string/strcmp.c"
  "$upstream/src/string/strnlen.c"
  "$upstream/src/string/memmove.c"
  "$upstream/src/string/memset.c"
  "$repo_root/musl/pachaos/syscall/thread_area.c"
)

object_path_for() {
  local src="$1"
  local rel="${src#$repo_root/}"
  rel="${rel//\//__}"
  rel="${rel//./_}"
  printf '%s/libc/%s.o' "$obj_dir" "$rel"
}

libc_objects=()
for src in "${libc_sources[@]}"; do
  obj="$(object_path_for "$src")"
  "$cc" "${common_cflags[@]}" -c "$src" -o "$obj"
  libc_objects+=("$obj")
done

"$ar" rcs "$sysroot/usr/lib/libc.a" "${libc_objects[@]}"

for crt in crt1 crti crtn Scrt1 rcrt1; do
  "$cc" "${common_cflags[@]}" -c "$upstream/crt/$crt.c" -o "$sysroot/usr/lib/$crt.o"
done

app_cflags=(
  -target "$target"
  --sysroot "$sysroot"
  -std=c99
  -fno-stack-protector
  -fno-plt
  -fPIE
  -mno-red-zone
  -O2
  -Wall
  -Wextra
  -D_XOPEN_SOURCE=700
)

"$cc" \
  "${app_cflags[@]}" \
  -c "$repo_root/musl/pachaos/smoke/hello.c" \
  -o "$obj_dir/app/hello.o"

"$cc" \
  -target "$target" \
  --sysroot "$sysroot" \
  -nostdlib \
  -nostartfiles \
  -static \
  -fuse-ld=lld \
  -fno-stack-protector \
  -fno-plt \
  -fPIE \
  -mno-red-zone \
  -Wl,-e,_start \
  -Wl,-pie \
  -Wl,--no-dynamic-linker \
  -Wl,-z,common-page-size=4096 \
  -Wl,-z,max-page-size=4096 \
  "$sysroot/usr/lib/rcrt1.o" \
  "$sysroot/usr/lib/crti.o" \
  "$obj_dir/app/hello.o" \
  -Wl,--start-group \
  "$sysroot/usr/lib/libc.a" \
  -Wl,--end-group \
  "$sysroot/usr/lib/crtn.o" \
  -o "$out_dir/hello-libc-scaffold.elf"

echo "$out_dir/hello-libc-scaffold.elf"
