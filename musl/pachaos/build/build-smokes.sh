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
app_src="${PACHAOS_MUSL_APP_SOURCE:-$repo_root/musl/pachaos/smoke/hello.c}"
app_out="${1:-$out_dir/hello-libc-scaffold.elf}"
extra_sources="${PACHAOS_MUSL_EXTRA_SOURCES:-}"
extra_include_dirs="${PACHAOS_MUSL_EXTRA_INCLUDE_DIRS:-}"
extra_cflags="${PACHAOS_MUSL_EXTRA_CFLAGS:-}"
static_pie="${PACHAOS_MUSL_STATIC_PIE:-1}"

rm -rf "$obj_dir" "$sysroot"
mkdir -p \
  "$out_dir" \
  "$obj_dir/include/bits" \
  "$obj_dir/src/internal" \
  "$obj_dir/libc" \
  "$obj_dir/app" \
  "$sysroot/usr/include" \
  "$sysroot/usr/lib"
mkdir -p "$(dirname "$app_out")"

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
  -D__pachaos__
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
  "$upstream/src/env/getenv.c"
  "$upstream/src/env/putenv.c"
  "$upstream/src/env/setenv.c"
  "$upstream/src/env/unsetenv.c"
  "$upstream/src/env/__init_tls.c"
  "$upstream/src/thread/default_attr.c"
  "$upstream/src/thread/__syscall_cp.c"
  "$upstream/src/internal/syscall_ret.c"
  "$upstream/src/errno/__errno_location.c"
  "$upstream/src/errno/strerror.c"
  "$upstream/src/exit/assert.c"
  "$upstream/src/exit/exit.c"
  "$upstream/src/exit/_Exit.c"
  "$upstream/src/exit/atexit.c"
  "$repo_root/musl/pachaos/cli/pachaos_cli_shim.c"
  "$upstream/src/unistd/read.c"
  "$upstream/src/unistd/write.c"
  "$upstream/src/unistd/close.c"
  "$upstream/src/unistd/access.c"
  "$upstream/src/unistd/getcwd.c"
  "$upstream/src/unistd/readlink.c"
  "$upstream/src/unistd/readv.c"
  "$upstream/src/unistd/writev.c"
  "$upstream/src/unistd/dup3.c"
  "$upstream/src/unistd/_exit.c"
  "$upstream/src/unistd/pread.c"
  "$upstream/src/unistd/pwrite.c"
  "$upstream/src/unistd/ftruncate.c"
  "$upstream/src/unistd/unlink.c"
  "$upstream/src/unistd/isatty.c"
  "$upstream/src/select/poll.c"
  "$upstream/src/time/nanosleep.c"
  "$upstream/src/time/clock_nanosleep.c"
  "$upstream/src/unistd/sleep.c"
  "$upstream/src/misc/ioctl.c"
  "$upstream/src/misc/basename.c"
  "$upstream/src/misc/dirname.c"
  "$upstream/src/misc/realpath.c"
  "$upstream/src/stat/fstat.c"
  "$upstream/src/stat/fstatat.c"
  "$upstream/src/stat/lstat.c"
  "$upstream/src/stat/stat.c"
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
  "$upstream/src/malloc/libc_calloc.c"
  "$upstream/src/malloc/free.c"
  "$upstream/src/malloc/realloc.c"
  "$upstream/src/malloc/reallocarray.c"
  "$upstream/src/malloc/replaced.c"
  "$upstream/src/time/clock_gettime.c"
  "$upstream/src/time/timespec_get.c"
  "$upstream/src/linux/eventfd.c"
  "$upstream/src/linux/timerfd.c"
  "$upstream/src/linux/brk.c"
  "$upstream/src/linux/sbrk.c"
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
  "$upstream/src/stdio/__uflow.c"
  "$upstream/src/stdio/fflush.c"
  "$upstream/src/stdio/fclose.c"
  "$upstream/src/stdio/fgetc.c"
  "$upstream/src/stdio/getc.c"
  "$upstream/src/stdio/feof.c"
  "$upstream/src/stdio/fgets.c"
  "$upstream/src/stdio/ferror.c"
  "$upstream/src/stdio/fopen.c"
  "$upstream/src/stdio/freopen.c"
  "$upstream/src/stdio/fread.c"
  "$upstream/src/stdio/clearerr.c"
  "$upstream/src/stdio/remove.c"
  "$upstream/src/stdio/rename.c"
  "$upstream/src/stdio/rewind.c"
  "$upstream/src/stdio/setvbuf.c"
  "$upstream/src/stdio/tmpfile.c"
  "$upstream/src/stdio/tmpnam.c"
  "$upstream/src/stdio/ungetc.c"
  "$upstream/src/stdio/fseek.c"
  "$upstream/src/stdio/ftell.c"
  "$upstream/src/stdio/__fdopen.c"
  "$upstream/src/stdio/__fmodeflags.c"
  "$upstream/src/stdio/__toread.c"
  "$upstream/src/stdio/fprintf.c"
  "$upstream/src/stdio/fputc.c"
  "$upstream/src/stdio/fputs.c"
  "$upstream/src/stdio/fwrite.c"
  "$upstream/src/stdio/ofl.c"
  "$upstream/src/stdio/ofl_add.c"
  "$upstream/src/stdio/open_memstream.c"
  "$upstream/src/stdio/printf.c"
  "$upstream/src/stdio/puts.c"
  "$upstream/src/stdio/snprintf.c"
  "$upstream/src/stdio/stderr.c"
  "$upstream/src/stdio/stdout.c"
  "$upstream/src/stdio/stdin.c"
  "$upstream/src/stdio/vfprintf.c"
  "$upstream/src/stdio/vsnprintf.c"
  "$upstream/src/locale/__lctrans.c"
  "$upstream/src/locale/__mo_lookup.c"
  "$upstream/src/locale/c_locale.c"
  "$upstream/src/locale/langinfo.c"
  "$upstream/src/locale/localeconv.c"
  "$upstream/src/locale/locale_map.c"
  "$upstream/src/locale/setlocale.c"
  "$upstream/src/locale/strcoll.c"
  "$upstream/src/multibyte/internal.c"
  "$upstream/src/multibyte/mbtowc.c"
  "$upstream/src/multibyte/wcrtomb.c"
  "$upstream/src/multibyte/wctomb.c"
  "$upstream/src/math/__fpclassifyl.c"
  "$upstream/src/math/__signbitl.c"
  "$upstream/src/math/__math_divzero.c"
  "$upstream/src/math/__math_invalid.c"
  "$upstream/src/math/__math_oflow.c"
  "$upstream/src/math/__math_uflow.c"
  "$upstream/src/math/__math_xflow.c"
  "$upstream/src/math/__cos.c"
  "$upstream/src/math/__rem_pio2.c"
  "$upstream/src/math/__rem_pio2_large.c"
  "$upstream/src/math/__sin.c"
  "$upstream/src/math/__tan.c"
  "$upstream/src/math/acos.c"
  "$upstream/src/math/asin.c"
  "$upstream/src/math/atan.c"
  "$upstream/src/math/atan2.c"
  "$upstream/src/math/ceil.c"
  "$upstream/src/math/cos.c"
  "$upstream/src/math/exp.c"
  "$upstream/src/math/exp_data.c"
  "$upstream/src/math/floor.c"
  "$upstream/src/math/fmod.c"
  "$upstream/src/math/fmodl.c"
  "$upstream/src/math/frexp.c"
  "$upstream/src/math/frexpl.c"
  "$upstream/src/math/ldexp.c"
  "$upstream/src/math/log.c"
  "$upstream/src/math/log10.c"
  "$upstream/src/math/log2.c"
  "$upstream/src/math/log2_data.c"
  "$upstream/src/math/log_data.c"
  "$upstream/src/math/pow.c"
  "$upstream/src/math/pow_data.c"
  "$upstream/src/math/scalbn.c"
  "$upstream/src/math/scalbnl.c"
  "$upstream/src/math/sin.c"
  "$upstream/src/math/tan.c"
  "$upstream/src/string/memcpy.c"
  "$upstream/src/string/bcmp.c"
  "$upstream/src/string/memcmp.c"
  "$upstream/src/string/memchr.c"
  "$upstream/src/string/memrchr.c"
  "$upstream/src/string/strchr.c"
  "$upstream/src/string/strchrnul.c"
  "$upstream/src/string/strcspn.c"
  "$upstream/src/string/strpbrk.c"
  "$upstream/src/string/strspn.c"
  "$upstream/src/string/strdup.c"
  "$upstream/src/string/strndup.c"
  "$upstream/src/string/strcpy.c"
  "$upstream/src/string/stpcpy.c"
  "$upstream/src/string/strncat.c"
  "$upstream/src/string/strncpy.c"
  "$upstream/src/string/stpncpy.c"
  "$upstream/src/string/strrchr.c"
  "$upstream/src/string/strstr.c"
  "$upstream/src/string/strtok.c"
  "$upstream/src/string/strlen.c"
  "$upstream/src/string/strncasecmp.c"
  "$upstream/src/string/strcmp.c"
  "$upstream/src/string/strncmp.c"
  "$upstream/src/string/strnlen.c"
  "$upstream/src/string/wcschr.c"
  "$upstream/src/string/wcslen.c"
  "$upstream/src/string/memmove.c"
  "$upstream/src/string/memset.c"
  "$upstream/src/ctype/isalnum.c"
  "$upstream/src/ctype/isblank.c"
  "$upstream/src/ctype/iscntrl.c"
  "$upstream/src/ctype/ispunct.c"
  "$upstream/src/ctype/isspace.c"
  "$upstream/src/ctype/isxdigit.c"
  "$upstream/src/ctype/iswalnum.c"
  "$upstream/src/ctype/iswalpha.c"
  "$upstream/src/ctype/iswblank.c"
  "$upstream/src/ctype/iswcntrl.c"
  "$upstream/src/ctype/iswctype.c"
  "$upstream/src/ctype/iswdigit.c"
  "$upstream/src/ctype/iswgraph.c"
  "$upstream/src/ctype/iswlower.c"
  "$upstream/src/ctype/iswprint.c"
  "$upstream/src/ctype/iswpunct.c"
  "$upstream/src/ctype/iswspace.c"
  "$upstream/src/ctype/iswupper.c"
  "$upstream/src/ctype/iswxdigit.c"
  "$upstream/src/ctype/tolower.c"
  "$upstream/src/ctype/towctrans.c"
  "$upstream/src/ctype/toupper.c"
  "$upstream/src/stdlib/bsearch.c"
  "$upstream/src/stdlib/qsort.c"
  "$upstream/src/stdlib/qsort_nr.c"
  "$upstream/src/stdlib/strtod.c"
  "$upstream/src/stdlib/strtol.c"
  "$upstream/src/internal/floatscan.c"
  "$upstream/src/internal/intscan.c"
  "$upstream/src/internal/shgetc.c"
  "$upstream/src/fcntl/open.c"
  "$upstream/src/fcntl/openat.c"
  "$upstream/src/fcntl/fcntl.c"
  "$upstream/src/linux/getdents.c"
  "$upstream/src/regex/fnmatch.c"
  "$upstream/src/regex/glob.c"
  "$upstream/src/dirent/closedir.c"
  "$upstream/src/dirent/opendir.c"
  "$upstream/src/dirent/readdir.c"
  "$upstream/src/process/execve.c"
  "$upstream/src/time/clock.c"
  "$upstream/src/time/asctime_r.c"
  "$upstream/src/time/ctime_r.c"
  "$upstream/src/time/__map_file.c"
  "$upstream/src/time/__month_to_secs.c"
  "$upstream/src/time/__secs_to_tm.c"
  "$upstream/src/time/__tm_to_secs.c"
  "$upstream/src/time/__tz.c"
  "$upstream/src/time/__utc.c"
  "$upstream/src/time/__year_to_secs.c"
  "$upstream/src/time/difftime.c"
  "$upstream/src/time/gmtime.c"
  "$upstream/src/time/gmtime_r.c"
  "$upstream/src/time/localtime.c"
  "$upstream/src/time/localtime_r.c"
  "$upstream/src/time/mktime.c"
  "$upstream/src/time/strftime.c"
  "$upstream/src/time/time.c"
  "$upstream/src/temp/mkostemps.c"
  "$upstream/src/temp/mkstemp.c"
  "$upstream/src/setjmp/x86_64/setjmp.s"
  "$upstream/src/setjmp/x86_64/longjmp.s"
  "$upstream/src/thread/pachaos/filed_runtime.c"
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
  -mno-red-zone
  -O2
  -Wall
  -Wextra
  -D_XOPEN_SOURCE=700
)
if [ "$static_pie" != "0" ]; then
  app_cflags+=("-fPIE")
else
  app_cflags+=("-fno-pie")
fi
for include_dir in $extra_include_dirs; do
  app_cflags+=("-I" "$include_dir")
done
for flag in $extra_cflags; do
  app_cflags+=("$flag")
done

"$cc" \
  "${app_cflags[@]}" \
  -c "$app_src" \
  -o "$obj_dir/app/hello.o"

app_objects=("$obj_dir/app/hello.o")
extra_index=0
for src in $extra_sources; do
  obj="$obj_dir/app/extra_${extra_index}.o"
  "$cc" \
    "${app_cflags[@]}" \
    -c "$src" \
    -o "$obj"
  app_objects+=("$obj")
  extra_index=$((extra_index + 1))
done

link_flags=(
  -target "$target"
  --sysroot "$sysroot"
  -nostdlib
  -nostartfiles
  -static
  -fuse-ld=lld
  -fno-stack-protector
  -fno-plt
  -mno-red-zone
  -Wl,-e,_start
  -Wl,--no-dynamic-linker
  -Wl,-z,common-page-size=4096
  -Wl,-z,max-page-size=4096
)
if [ "$static_pie" != "0" ]; then
  link_flags+=("-fPIE" "-Wl,-pie")
  crt_start="$sysroot/usr/lib/rcrt1.o"
else
  link_flags+=("-fno-pie" "-Wl,-no-pie")
  crt_start="$sysroot/usr/lib/crt1.o"
fi

"$cc" \
  "${link_flags[@]}" \
  "$crt_start" \
  "$sysroot/usr/lib/crti.o" \
  "${app_objects[@]}" \
  -Wl,--start-group \
  "$sysroot/usr/lib/libc.a" \
  -Wl,--end-group \
  "$sysroot/usr/lib/crtn.o" \
  -o "$app_out"

echo "$app_out"
