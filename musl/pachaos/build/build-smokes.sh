#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../../.." && pwd)"
out_dir="$repo_root/.artifacts/musl-pachaos"
cc="${CAPOS_FREESTANDING_CC:-clang}"
ar="${CAPOS_AR:-ar}"
target="${PACHAOS_MUSL_DRIVER_TARGET:-x86_64-linux-musl}"
upstream="$repo_root/musl/upstream"
app_src="${PACHAOS_MUSL_APP_SOURCE:-$repo_root/musl/pachaos/smoke/hello.c}"
app_out="${1:-$out_dir/hello-libc-scaffold.elf}"
extra_sources="${PACHAOS_MUSL_EXTRA_SOURCES:-}"
extra_include_dirs="${PACHAOS_MUSL_EXTRA_INCLUDE_DIRS:-}"
extra_cflags="${PACHAOS_MUSL_EXTRA_CFLAGS:-}"
libc_extra_cflags="${PACHAOS_MUSL_LIBC_EXTRA_CFLAGS:-}"
extra_link_flags="${PACHAOS_MUSL_EXTRA_LINK_FLAGS:-}"
extra_link_inputs="${PACHAOS_MUSL_EXTRA_LINK_INPUTS:-}"
static_pie="${PACHAOS_MUSL_STATIC_PIE:-1}"
case "$app_out" in /*) ;; *) app_out="$PWD/$app_out" ;; esac
depfile="$app_out.d"

# Each output owns its intermediate tree, so concurrent builds of different
# ELFs (pacgo recipes, Ninja jobs) never delete or relink each other's files.
work_key="${app_out#"$repo_root"/}"
work_key="${work_key//\//__}"
work_dir="$out_dir/work/$work_key"
obj_dir="$work_dir/obj"

rm -rf "$work_dir" "$depfile"
mkdir -p "$obj_dir/app" "$(dirname "$app_out")"

# PACHAOS_MUSL_LIBC_DIR names a libc already built by build-libc.sh (the
# musl_libc pacgo recipes); otherwise this ELF builds its own private copy.
direct_deps=("${BASH_SOURCE[0]}")
if [ -n "${PACHAOS_MUSL_LIBC_DIR:-}" ]; then
  libc_dir="$PACHAOS_MUSL_LIBC_DIR"
  case "$libc_dir" in /*) ;; *) libc_dir="$repo_root/$libc_dir" ;; esac
  if [ -n "$libc_extra_cflags" ]; then
    echo "build-smokes.sh: PACHAOS_MUSL_LIBC_EXTRA_CFLAGS cannot change a prebuilt libc ($libc_dir)" >&2
    exit 2
  fi
  if [ ! -f "$libc_dir/config" ]; then
    echo "build-smokes.sh: $libc_dir is not a built libc; build its musl_libc recipe first" >&2
    exit 2
  fi
  if ! grep -qxF "target=$target" "$libc_dir/config"; then
    echo "build-smokes.sh: $libc_dir was not built for $target" >&2
    exit 2
  fi
  private_libc=0
else
  libc_dir="$work_dir/libc"
  # shellcheck disable=SC2086 # flag list, split like the other flag variables
  bash "$script_dir/build-libc.sh" "$libc_dir" $libc_extra_cflags
  private_libc=1
fi
sysroot="$libc_dir/sysroot"

# Every translation unit writes its own depfile; .s sources are not
# preprocessed, so they depend only on themselves.
tu_depfiles=()
compile() {
  local src="$1" obj="$2"
  shift 2
  case "$src" in
    *.s)
      "$cc" "$@" -c "$src" -o "$obj"
      direct_deps+=("$src")
      ;;
    *)
      "$cc" "$@" -MD -MF "$obj.d" -c "$src" -o "$obj"
      tu_depfiles+=("$obj.d")
      ;;
  esac
}

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

compile "$app_src" "$obj_dir/app/hello.o" "${app_cflags[@]}"

app_objects=("$obj_dir/app/hello.o")
extra_index=0
for src in $extra_sources; do
  obj="$obj_dir/app/extra_${extra_index}.o"
  compile "$src" "$obj" "${app_cflags[@]}"
  app_objects+=("$obj")
  extra_index=$((extra_index + 1))
done

extra_link_input_array=()
for link_input in $extra_link_inputs; do
  extra_link_input_array+=("$link_input")
  direct_deps+=("$link_input")
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
for flag in $extra_link_flags; do
  link_flags+=("$flag")
done

"$cc" \
  "${link_flags[@]}" \
  "$crt_start" \
  "$sysroot/usr/lib/crti.o" \
  "${app_objects[@]}" \
  -Wl,--start-group \
  "${extra_link_input_array[@]}" \
  "$sysroot/usr/lib/libc.a" \
  -Wl,--end-group \
  "$sysroot/usr/lib/crtn.o" \
  -o "$app_out"

if [ "$private_libc" = 1 ]; then
  # The private libc lives in the work tree; depend on what it was built from.
  tu_depfiles+=("$libc_dir/libc.d")
else
  direct_deps+=("$crt_start" "$sysroot/usr/lib/crti.o" "$sysroot/usr/lib/libc.a" "$sysroot/usr/lib/crtn.o")
fi

# Work-tree paths are rewritten to the files they stand for, so the depfile
# names only files that outlive this run. Sysroot header links resolve to
# their musl sources; the generated sysroot headers of a prebuilt libc belong
# to its recipe, and those of a private libc to its generator inputs.
source_for() {
  local path="$1"
  case "$path" in
    "$sysroot/usr/include/bits/alltypes.h" | "$sysroot/usr/include/bits/syscall.h")
      [ "$private_libc" = 1 ] || printf '%s\n' "$path"
      ;;
    "$libc_dir/"* | "$work_dir/"*)
      if [ -L "$path" ]; then
        readlink -f -- "$path"
      elif [ "$private_libc" = 0 ] && [ "${path#"$libc_dir/"}" != "$path" ]; then
        printf '%s\n' "$path"
      else
        echo "build-smokes.sh: no source for generated dependency $path" >&2
        return 1
      fi
      ;;
    *)
      printf '%s\n' "$path"
      ;;
  esac
}

if grep -q '\\ ' "${tu_depfiles[@]}"; then
  echo "build-smokes.sh: dependency paths with spaces are not supported" >&2
  exit 1
fi
mapfile -t raw_deps < <(
  {
    sed -e 's/\\$//' "${tu_depfiles[@]}" | tr -s ' \t' '\n\n' | grep -v -e ':$' -e '^$'
    printf '%s\n' "${direct_deps[@]}" "${BASH_SOURCE[0]}"
  } | sort -u | xargs -d '\n' realpath -m -s --
)
{
  printf '%s:' "$app_out"
  for dep in "${raw_deps[@]}"; do
    source_for "$dep"
  done | sort -u | while IFS= read -r dep; do
    printf ' \\\n  %s' "$dep"
  done
  printf '\n'
} > "$depfile.tmp"
mv "$depfile.tmp" "$depfile"

echo "$app_out"
