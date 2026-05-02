#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "$0")/.." && pwd)"
src_dir="$root_dir/.artifacts/src"
version="1.36.1"
archive="$src_dir/busybox-$version.tar.bz2"
build_dir="$src_dir/busybox-$version"
output="$root_dir/userland/fixtures/busybox.elf"
stamp="$build_dir/.capabilityos-built"
cc_stamp="$build_dir/.capabilityos-cc"
config_stamp="$build_dir/.capabilityos-config"
config_id="minimal-ash-coreutils-v3"

mkdir -p "$src_dir" "$root_dir/userland/fixtures"

if [ ! -f "$archive" ]; then
  wget -O "$archive" "https://busybox.net/downloads/busybox-$version.tar.bz2"
fi

if command -v musl-gcc >/dev/null 2>&1; then
  cc=musl-gcc
elif command -v musl-clang >/dev/null 2>&1; then
  cc=musl-clang
else
  echo "missing musl toolchain: install musl-tools in WSL" >&2
  exit 1
fi

if [ ! -d "$build_dir" ]; then
  tar -xjf "$archive" -C "$src_dir"
fi

cd "$build_dir"

if [ -f "$stamp" ] && [ -x busybox ] && [ "$(cat "$cc_stamp" 2>/dev/null || true)" = "$cc" ] && [ "$(cat "$config_stamp" 2>/dev/null || true)" = "$config_id" ]; then
  if [ ! -f "$output" ] || [ busybox -nt "$output" ]; then
    cp busybox "$output"
  fi
  exit 0
fi

make allnoconfig

set_config() {
  key="$1"
  value="$2"
  if grep -q "^$key=" .config; then
    sed -i "s/^$key=.*/$key=$value/" .config
  elif grep -q "^# $key is not set" .config; then
    sed -i "s/^# $key is not set/$key=$value/" .config
  else
    printf '%s=%s\n' "$key" "$value" >> .config
  fi
}

disable_config() {
  key="$1"
  if grep -q "^$key=" .config; then
    sed -i "s/^$key=.*/# $key is not set/" .config
  elif ! grep -q "^# $key is not set" .config; then
    printf '# %s is not set\n' "$key" >> .config
  fi
}

disable_config CONFIG_STATIC
set_config CONFIG_PIE y
set_config CONFIG_BUSYBOX y
set_config CONFIG_SH_IS_ASH y
set_config CONFIG_BASH_IS_NONE y
disable_config CONFIG_FEATURE_SH_STANDALONE
set_config CONFIG_ASH y
set_config CONFIG_ASH_ECHO y
set_config CONFIG_CAT y
set_config CONFIG_ECHO y
set_config CONFIG_LS y
set_config CONFIG_TRUE y
set_config CONFIG_FALSE y
set_config CONFIG_TEST y

set +o pipefail
yes "" | make oldconfig
oldconfig_status="${PIPESTATUS[1]}"
set -o pipefail
if [ "$oldconfig_status" -ne 0 ]; then
  exit "$oldconfig_status"
fi
make -j"$(nproc)" CC="$cc" EXTRA_LDFLAGS="-Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 -Wl,-rpath,/lib" busybox
cp busybox "$output"
printf '%s\n' "$cc" > "$cc_stamp"
printf '%s\n' "$config_id" > "$config_stamp"
touch "$stamp"
