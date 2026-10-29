#!/usr/bin/env bash
set -euo pipefail

# Prints a clean Zydis tree (no .git) at the pinned revisions. The clone is
# only a download cache; the tree is replaced only when the pin changes, so
# repeated calls neither touch the network nor change file timestamps.
# --refresh (the zydis_source pacgo recipe) always re-exports the tree.
refresh=0
if [[ "${1:-}" == --refresh ]]; then
  refresh=1
fi
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
root="$repo_root/.artifacts/third_party/zydis-v4.1.1"
source_dir="$root/source"
zycore_dir="$root/zycore"
tree_dir="$root/tree"
zydis_commit="a2278f1d254e492f6a6b39f6cb5d1f5d515659dc"
zycore_commit="0b2432ced0884fd152b471d97ecf0258ff4d859f"
revision="zydis $zydis_commit zycore $zycore_commit"

mkdir -p "$root"
# Concurrent callers (filed, the LPR decoder, tests) share one download.
exec {lock_fd}>"$root/.lock"
flock "$lock_fd"

if [[ "$refresh" == 0 && -f "$tree_dir/.revision" && "$(cat "$tree_dir/.revision")" == "$revision" ]]; then
  printf '%s\n' "$tree_dir"
  exit 0
fi

# fetch_pinned <clone> <url> <commit>: a blobless clone checked out at commit.
fetch_pinned() {
  local clone="$1" url="$2" commit="$3"
  if [[ ! -d "$clone/.git" ]]; then
    git clone --filter=blob:none --no-checkout "$url" "$clone"
  fi
  if [[ "$(git -C "$clone" rev-parse HEAD 2>/dev/null || true)" != "$commit" ]]; then
    git -C "$clone" fetch --depth=1 origin "$commit"
    git -C "$clone" checkout --detach "$commit"
  fi
}
fetch_pinned "$source_dir" https://github.com/zyantific/zydis.git "$zydis_commit"
# Zycore is Zydis's dependencies/zycore submodule, pinned and fetched directly.
fetch_pinned "$zycore_dir" https://github.com/zyantific/zycore-c "$zycore_commit"

actual_zydis="$(git -C "$source_dir" rev-parse HEAD)"
actual_zycore="$(git -C "$zycore_dir" rev-parse HEAD)"
if [[ "$actual_zydis" != "$zydis_commit" || "$actual_zycore" != "$zycore_commit" ]]; then
  echo "unexpected Zydis source revision" >&2
  echo "Zydis: expected $zydis_commit, got $actual_zydis" >&2
  echo "Zycore: expected $zycore_commit, got $actual_zycore" >&2
  exit 1
fi

rm -rf "$tree_dir.tmp"
git -C "$source_dir" checkout-index --all --prefix="$tree_dir.tmp/"
git -C "$zycore_dir" checkout-index --all \
  --prefix="$tree_dir.tmp/dependencies/zycore/"
printf '%s\n' "$revision" > "$tree_dir.tmp/.revision"
rm -rf "$tree_dir"
mv "$tree_dir.tmp" "$tree_dir"

printf '%s\n' "$tree_dir"
