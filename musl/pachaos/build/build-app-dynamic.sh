#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../../.." && pwd)"

runtime_dir="$repo_root/.artifacts/musl-pachaos-runtime"
runtime_install="$runtime_dir/install"
runtime_lib="$runtime_install/usr/lib"
out_dir="$repo_root/.artifacts/musl-pachaos-dynamic"
cc="${CAPOS_FREESTANDING_CC:-clang}"
target="${PACHAOS_MUSL_DRIVER_TARGET:-x86_64-linux-musl}"
upstream="$repo_root/musl/upstream"
app_src="${PACHAOS_MUSL_APP_SOURCE:-$repo_root/musl/pachaos/smoke/hello.c}"
app_out="${1:-$out_dir/hello-dynamic.elf}"
extra_sources="${PACHAOS_MUSL_EXTRA_SOURCES:-}"
extra_include_dirs="${PACHAOS_MUSL_EXTRA_INCLUDE_DIRS:-}"
extra_cflags="${PACHAOS_MUSL_EXTRA_CFLAGS:-}"
extra_link_inputs="${PACHAOS_MUSL_EXTRA_LINK_INPUTS:-}"
pie="${PACHAOS_MUSL_DYNAMIC_PIE:-1}"
build_id="$(printf '%s' "$app_out" | sed 's#[^A-Za-z0-9_.-]#_#g')"
obj_dir="$out_dir/obj/$build_id"
sysroot="$out_dir/sysroot/$build_id"

if [ ! -f "$runtime_lib/libc.so" ] ||
   [ ! -f "$runtime_lib/Scrt1.o" ] ||
   [ ! -f "$runtime_lib/crt1.o" ]; then
  bash "$repo_root/musl/pachaos/build/build-runtime.sh"
fi

rm -rf "$obj_dir" "$sysroot"
mkdir -p \
  "$out_dir" \
  "$obj_dir/include/bits" \
  "$obj_dir/src/internal" \
  "$obj_dir/app" \
  "$sysroot/usr/include" \
  "$sysroot/usr/lib" \
  "$(dirname "$app_out")"

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

cp "$runtime_lib/crt1.o" "$sysroot/usr/lib/crt1.o"
cp "$runtime_lib/crti.o" "$sysroot/usr/lib/crti.o"
cp "$runtime_lib/crtn.o" "$sysroot/usr/lib/crtn.o"
cp "$runtime_lib/Scrt1.o" "$sysroot/usr/lib/Scrt1.o"
cp "$runtime_lib/libc.so" "$sysroot/usr/lib/libc.so"

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
if [ "$pie" != "0" ]; then
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

"$cc" "${app_cflags[@]}" -c "$app_src" -o "$obj_dir/app/main.o"

app_objects=("$obj_dir/app/main.o")
extra_index=0
for src in $extra_sources; do
  obj="$obj_dir/app/extra_${extra_index}.o"
  "$cc" "${app_cflags[@]}" -c "$src" -o "$obj"
  app_objects+=("$obj")
  extra_index=$((extra_index + 1))
done

extra_link_input_array=()
for link_input in $extra_link_inputs; do
  extra_link_input_array+=("$link_input")
done

link_flags=(
  -target "$target"
  --sysroot "$sysroot"
  -nostdlib
  -nostartfiles
  -fuse-ld=lld
  -fno-stack-protector
  -fno-plt
  -mno-red-zone
  -Wl,-e,_start
  -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1
  -Wl,-z,common-page-size=4096
  -Wl,-z,max-page-size=4096
  -L "$sysroot/usr/lib"
)
if [ "$pie" != "0" ]; then
  link_flags+=("-fPIE" "-Wl,-pie")
  crt_start="$sysroot/usr/lib/Scrt1.o"
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
  "${extra_link_input_array[@]}" \
  -lc \
  -Wl,--end-group \
  "$sysroot/usr/lib/crtn.o" \
  -o "$app_out"

echo "$app_out"
