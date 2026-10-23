#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"
bash pack/scripts/build_lpr_pthread_smoke.sh \
  .artifacts/filecache-tests/guest tests/lpr_snapshot_cache_guest.c
"${CC:-/usr/bin/clang}" -target x86_64-linux-musl -fPIC -O2 -shared -nostdlib \
  -Wl,-soname,snapshot-dso.so tests/lpr_snapshot_cache_dso.c \
  -o .artifacts/filecache-tests/snapshot-dso.so
# Run guest.dynamic.elf EXT4_TEST_FILE EXT4_DSO_PATH in a disposable guest.
# /tmp is normally tmpfs and intentionally cannot exercise the cookie cache.
