#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "$0")/.." && pwd)"
src_dir="$root_dir/.artifacts/src"
version="0.4.0"
archive_name="coreutils-$version-source.tar.gz"
archive="$src_dir/$archive_name"
extract_dir="$src_dir/coreutils-$version-source"
default_extract_dir="$src_dir/coreutils-$version"
output="$root_dir/userland/fixtures/uutils-coreutils-hot.elf"
target="x86_64-unknown-linux-musl"
expected_sha256="5f0c3f97b807e72edccc844c6a685ec9862199f16a665df07de5b1d20ec21233"
features="ls cat basename dirname true false echo pwd test"

mkdir -p "$src_dir" "$(dirname "$output")"

if [ ! -f "$archive" ]; then
  if command -v curl >/dev/null 2>&1; then
    curl -L -o "$archive" "https://github.com/uutils/coreutils/archive/refs/tags/$version.tar.gz"
  else
    wget -O "$archive" "https://github.com/uutils/coreutils/archive/refs/tags/$version.tar.gz"
  fi
fi

actual_sha256="$(sha256sum "$archive" | awk '{print $1}')"
if [ "$actual_sha256" != "$expected_sha256" ]; then
  echo "uutils source archive sha256 mismatch: $actual_sha256" >&2
  exit 1
fi

if [ ! -f "$extract_dir/Cargo.toml" ]; then
  rm -rf "$extract_dir" "$default_extract_dir"
  tar -xzf "$archive" -C "$src_dir"
  mv "$default_extract_dir" "$extract_dir"
fi

if [ -f "$HOME/.cargo/env" ]; then
  # shellcheck disable=SC1091
  . "$HOME/.cargo/env"
fi

if ! command -v cargo >/dev/null 2>&1 || ! command -v rustup >/dev/null 2>&1; then
  echo "cargo/rustup not found in WSL; install rustup first" >&2
  exit 1
fi

rustup target add "$target"

RUSTFLAGS="${RUSTFLAGS:-} -C linker=rust-lld" \
  cargo +stable build \
    --manifest-path "$extract_dir/Cargo.toml" \
    --release \
    --target "$target" \
    --no-default-features \
    --features "$features"

cp "$extract_dir/target/$target/release/coreutils" "$output"
chmod +x "$output"
