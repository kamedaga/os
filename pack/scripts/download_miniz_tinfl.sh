#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
source_dir="$repo_root/.artifacts/third_party/miniz-3.0.2/source"
miniz_commit="293d4db1b7d0ffee9756d035b9ac6f7431ef8492"

mkdir -p "$(dirname "$source_dir")"
if [[ ! -d "$source_dir/.git" ]]; then
  git clone --filter=blob:none --no-checkout \
    https://github.com/richgel999/miniz.git "$source_dir"
fi

if [[ "$(git -C "$source_dir" rev-parse HEAD 2>/dev/null || true)" != "$miniz_commit" ]]; then
  git -C "$source_dir" fetch --depth=1 origin "$miniz_commit"
  git -C "$source_dir" checkout --detach "$miniz_commit"
fi

if [[ "$(git -C "$source_dir" rev-parse HEAD)" != "$miniz_commit" ]]; then
  echo "unexpected miniz source revision" >&2
  exit 1
fi

printf '%s\n' "$source_dir"
