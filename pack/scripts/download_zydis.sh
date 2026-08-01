#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
source_dir="$repo_root/.artifacts/third_party/zydis-v4.1.1/source"
zydis_commit="a2278f1d254e492f6a6b39f6cb5d1f5d515659dc"
zycore_commit="0b2432ced0884fd152b471d97ecf0258ff4d859f"

mkdir -p "$(dirname "$source_dir")"
if [[ ! -d "$source_dir/.git" ]]; then
  git clone --filter=blob:none --no-checkout \
    https://github.com/zyantific/zydis.git "$source_dir"
fi

if [[ "$(git -C "$source_dir" rev-parse HEAD 2>/dev/null || true)" != "$zydis_commit" ]]; then
  git -C "$source_dir" fetch --depth=1 origin "$zydis_commit"
  git -C "$source_dir" checkout --detach "$zydis_commit"
fi

git -C "$source_dir" submodule update --init --depth=1 dependencies/zycore

actual_zydis="$(git -C "$source_dir" rev-parse HEAD)"
actual_zycore="$(git -C "$source_dir/dependencies/zycore" rev-parse HEAD)"
if [[ "$actual_zydis" != "$zydis_commit" || "$actual_zycore" != "$zycore_commit" ]]; then
  echo "unexpected Zydis source revision" >&2
  echo "Zydis: expected $zydis_commit, got $actual_zydis" >&2
  echo "Zycore: expected $zycore_commit, got $actual_zycore" >&2
  exit 1
fi

printf '%s\n' "$source_dir"
