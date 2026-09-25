#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:-.artifacts/userland-fixtures/lpr-linux-musl-libc.so}"
case "$out" in /*) ;; *) out="$repo_root/$out" ;; esac
mirror="${PACHAOS_ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}"
cache="$repo_root/.artifacts/third_party/alpine-lpr-musl"
lock="$repo_root/tools/manifests/alpine-xfce-v3.22-x86_64.lock"

# Use the distribution package pinned by the desktop image.
# Never compile the native port or substitute a local libc implementation.
read -r section package version expected_sha256 < <(awk '$2 == "musl" { print }' "$lock")
[[ "$section" == main && "$package" == musl && -n "$expected_sha256" ]]
mkdir -p "$cache" "$(dirname "$out")"
apk="$cache/musl-$version.apk"
if [[ ! -f "$apk" ]]; then
  curl -fsSL "$mirror/v3.22/main/x86_64/musl-$version.apk" -o "$apk"
fi
printf '%s  %s\n' "$expected_sha256" "$apk" | sha256sum -c -
extract="$(mktemp -d "$cache/extract.XXXXXX")"
trap 'rm -rf -- "$extract"' EXIT
tar --warning=no-unknown-keyword -xzf "$apk" -C "$extract" lib/ld-musl-x86_64.so.1
cp "$extract/lib/ld-musl-x86_64.so.1" "$out"
chmod 0755 "$out"
printf 'Installed Alpine v3.22 musl-%s (no local patches):\n' "$version"
sha256sum "$out"
