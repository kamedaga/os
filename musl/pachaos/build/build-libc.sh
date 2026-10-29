#!/usr/bin/env bash
set -euo pipefail

# Builds the PachaOS musl libc once for one configuration: the sysroot headers,
# libc.a and the crt objects. Native ELFs compile against and link with it.
#   build-libc.sh <libc-dir> [extra libc cflags...]
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../../.." && pwd)"
libc_dir="${1:?usage: build-libc.sh <libc-dir> [extra libc cflags...]}"
shift
case "$libc_dir" in /*) ;; *) libc_dir="$PWD/$libc_dir" ;; esac
cc="${CAPOS_FREESTANDING_CC:-clang}"
ar="${CAPOS_AR:-ar}"
target="${PACHAOS_MUSL_DRIVER_TARGET:-x86_64-linux-musl}"
upstream="$repo_root/musl/upstream"
obj_dir="$libc_dir/obj"
sysroot="$libc_dir/sysroot"

case "$libc_dir" in
  "$repo_root/.artifacts/"?*) ;;
  *) echo "build-libc.sh: libc directory must be below $repo_root/.artifacts: $libc_dir" >&2; exit 2 ;;
esac
# The previous build stays aside until the end: sysroot files that come out
# identical get their old timestamps back, so Ninja in the consumers does not
# recompile against regenerated headers or relink an unchanged libc.a.
previous_dir="$libc_dir.previous"
rm -rf "$previous_dir"
if [ -d "$libc_dir" ]; then
  mv "$libc_dir" "$previous_dir"
fi
mkdir -p \
  "$obj_dir/include/bits" \
  "$obj_dir/src/internal" \
  "$obj_dir/libc" \
  "$sysroot/usr/include" \
  "$sysroot/usr/lib"

sed -f "$upstream/tools/mkalltypes.sed" \
  "$upstream/arch/pachaos/bits/alltypes.h.in" \
  "$upstream/include/alltypes.h.in" \
  > "$obj_dir/include/bits/alltypes.h"

cp "$upstream/arch/pachaos/bits/syscall.h.in" "$obj_dir/include/bits/syscall.h"
sed -n -e 's/__NR_/SYS_/p' "$upstream/arch/pachaos/bits/syscall.h.in" >> "$obj_dir/include/bits/syscall.h"
printf '#define VERSION "%s"\n' "$(cat "$upstream/VERSION")" > "$obj_dir/src/internal/version.h"

# The sysroot headers are per-file links, so only headers a translation unit
# actually opens are read, and each link names its source for the depfile.
cp -Rs "$upstream/include/." "$sysroot/usr/include/"
mkdir -p "$sysroot/usr/include/bits" "$sysroot/usr/include/pachaos"
cp -Rs "$upstream/arch/generic/bits/." "$sysroot/usr/include/bits/"
cp -Rs --remove-destination "$upstream/arch/pachaos/bits/." "$sysroot/usr/include/bits/"
cp --remove-destination "$obj_dir/include/bits/alltypes.h" "$sysroot/usr/include/bits/alltypes.h"
cp --remove-destination "$obj_dir/include/bits/syscall.h" "$sysroot/usr/include/bits/syscall.h"
ln -s "$repo_root/musl/pachaos/include/pachaos/abi.h" "$sysroot/usr/include/pachaos/abi.h"

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
  -I "$repo_root/musl/pachaos/syscall"
  -I "$repo_root/userland/filed/include"
  -I "$repo_root/userland/libipc/include"
  -I "$repo_root/kobox2/protocol/include"
  -I "$repo_root/kobox2/protocol/generated/include"
  -I "$upstream/arch/pachaos"
  -I "$upstream/arch/generic"
  -I "$obj_dir/src/internal"
  -I "$upstream/src/include"
  -I "$upstream/src/internal"
  -I "$obj_dir/include"
)
common_cflags+=("$@")


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
  "$upstream/src/stdio/fopencookie.c"
  "$upstream/src/stdio/ofl.c"
  "$upstream/src/stdio/ofl_add.c"
  "$upstream/src/stdio/open_memstream.c"
  "$upstream/src/stdio/printf.c"
  "$upstream/src/stdio/puts.c"
  "$upstream/src/stdio/snprintf.c"
  "$upstream/src/stdio/sprintf.c"
  "$upstream/src/stdio/stderr.c"
  "$upstream/src/stdio/stdout.c"
  "$upstream/src/stdio/stdin.c"
  "$upstream/src/stdio/vfprintf.c"
  "$upstream/src/stdio/vsnprintf.c"
  "$upstream/src/stdio/vsprintf.c"
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
  "$upstream/src/stdlib/atoi.c"
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
  "$repo_root/musl/pachaos/syscall/native.c"
  "$repo_root/kobox2/protocol/src/filesystem.c"
  "$repo_root/userland/filed/src/exec_payload.c"
  "$repo_root/musl/pachaos/syscall/thread_area.c"
)

object_path_for() {
  local src="$1"
  local rel="${src#$repo_root/}"
  rel="${rel//\//__}"
  rel="${rel//./_}"
  printf '%s/libc/%s.o' "$obj_dir" "$rel"
}


# Every translation unit writes its own depfile; .s sources are not
# preprocessed, so they depend only on themselves.
# The generated headers are written from these on every run.
direct_deps=(
  "$upstream/tools/mkalltypes.sed"
  "$upstream/arch/pachaos/bits/alltypes.h.in"
  "$upstream/include/alltypes.h.in"
  "$upstream/arch/pachaos/bits/syscall.h.in"
  "$upstream/VERSION"
  "${BASH_SOURCE[0]}"
)
libc_objects=()
compile_list=()
for src in "${libc_sources[@]}"; do
  obj="$(object_path_for "$src")"
  compile_list+=("$src" "$obj")
  libc_objects+=("$obj")
  case "$src" in *.s) direct_deps+=("$src") ;; esac
done
for crt in crt1 crti crtn Scrt1 rcrt1; do
  compile_list+=("$upstream/crt/$crt.c" "$obj_dir/libc/crt_$crt.o")
done

# GNU make runs the compiles so that every job takes its own jobserver token
# while it runs; a fixed reservation at start would leave a libc that starts
# beside another recipe with whatever tokens were free at that moment.
# make_word <variable> <word>: the word quoted for the recipe shell, with
# make's own "$" escaped.
make_word() {
  local -n make_word_result="$1"
  printf -v make_word_result '%q' "$2"
  make_word_result="${make_word_result//\$/\$\$}"
}
compile_command=""
for word in "$cc" "${common_cflags[@]}"; do
  make_word quoted "$word"
  compile_command+=" $quoted"
done
makefile="$obj_dir/compile.mk"
{
  printf '.PHONY: all\nall:'
  for ((index = 1; index < ${#compile_list[@]}; index += 2)); do
    make_word quoted "${compile_list[index]}"
    printf ' %s' "$quoted"
  done
  printf '\n'
  for ((index = 0; index < ${#compile_list[@]}; index += 2)); do
    make_word quoted_src "${compile_list[index]}"
    make_word quoted_obj "${compile_list[index + 1]}"
    printf '%s:\n' "$quoted_obj"
    case "$quoted_src" in
      *.s) printf '\t@%s -c %s -o $@\n' "$compile_command" "$quoted_src" ;;
      *) printf '\t@%s -MD -MF $@.d -c %s -o $@\n' "$compile_command" "$quoted_src" ;;
    esac
  done
} > "$makefile"
source "$repo_root/pack/scripts/lib/jobs.sh"
pacgo_make "${PACGO_BUILD_JOBS:-$(nproc)}" --no-print-directory -f "$makefile" all

"$ar" rcs "$sysroot/usr/lib/libc.a" "${libc_objects[@]}"
for crt in crt1 crti crtn Scrt1 rcrt1; do
  cp "$obj_dir/libc/crt_$crt.o" "$sysroot/usr/lib/$crt.o"
done
tu_depfiles=("$obj_dir"/libc/*.o.d)

# The configuration a consumer must match; build-smokes.sh checks it.
printf 'target=%s\ncflags=%s\n' "$target" "$*" > "$libc_dir/config"

# libc.d names every file this libc was built from. Generated headers are
# covered by their generator inputs and sysroot links by their targets.
if grep -q '\\ ' "${tu_depfiles[@]}"; then
  echo "build-libc.sh: dependency paths with spaces are not supported" >&2
  exit 1
fi
source_for() {
  local path="$1"
  case "$path" in
    "$obj_dir/include/bits/alltypes.h" | "$sysroot/usr/include/bits/alltypes.h" | \
    "$obj_dir/include/bits/syscall.h" | "$sysroot/usr/include/bits/syscall.h" | \
    "$obj_dir/src/internal/version.h")
      ;;
    "$libc_dir/"*)
      if [ ! -L "$path" ]; then
        echo "build-libc.sh: no source for generated dependency $path" >&2
        return 1
      fi
      readlink -f -- "$path"
      ;;
    *)
      printf '%s\n' "$path"
      ;;
  esac
}
mapfile -t raw_deps < <(
  {
    sed -e 's/\\$//' "${tu_depfiles[@]}" | tr -s ' \t' '\n\n' | grep -v -e ':$' -e '^$'
    printf '%s\n' "${direct_deps[@]}"
  } | sort -u | xargs -d '\n' realpath -m -s --
)
{
  printf '%s:' "$sysroot/usr/lib/libc.a"
  for dep in "${raw_deps[@]}"; do
    source_for "$dep"
  done | sort -u | while IFS= read -r dep; do
    printf ' \\\n  %s' "$dep"
  done
  printf '\n'
} > "$libc_dir/libc.d.tmp"
mv "$libc_dir/libc.d.tmp" "$libc_dir/libc.d"

if [ -d "$previous_dir" ]; then
  (
    cd "$libc_dir"
    find sysroot -type f -print0 | while IFS= read -r -d '' file; do
      if [ -f "$previous_dir/$file" ] && cmp -s "$file" "$previous_dir/$file"; then
        touch -r "$previous_dir/$file" "$file"
      fi
    done
  )
  rm -rf "$previous_dir"
fi
