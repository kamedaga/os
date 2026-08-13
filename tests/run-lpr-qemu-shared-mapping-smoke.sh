#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

if [[ "${SKIP_SYNC:-0}" != "1" ]]; then
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs
fi

.artifacts/bin/pacgo qemu-test \
  --timeout 30s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
  --send '/cmd/lpr_shared_mapping.elf write' \
  --expect 'SHMAP_FILE_COHERENCE=OK' \
  --expect 'SHMAP_FILE_MSYNC=OK' \
  --expect 'SHMAP_WRITE_DONE'

sleep 3

.artifacts/bin/pacgo qemu-test \
  --timeout 30s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=' \
  --send '/cmd/lpr_shared_mapping.elf verify' \
  --expect 'SHMAP_FILE_PERSISTENCE=OK' \
  --expect 'SHMAP_MEMFD_TWO_MAP=OK' \
  --expect 'SHMAP_PRIVATE_FILE_COW=OK' \
  --expect 'SHMAP_PRIVATE_SPLIT_COW=OK' \
  --expect 'SHMAP_MPROTECT_SHARED_RDWR=OK' \
  --expect 'SHMAP_MPROTECT_SHARED_RDONLY_DENIED=OK' \
  --expect 'SHMAP_MPROTECT_PRIVATE_RDONLY_COW=OK' \
  --expect 'SHMAP_MPROTECT_FUTURE_WRITE_DENIED=OK' \
  --expect 'SHMAP_MPROTECT_SHARED_RX=OK' \
  --expect 'SHMAP_MEMFD_FORK_BIDIR=OK' \
  --expect 'SHMAP_ANON_FORK_BIDIR=OK' \
  --expect 'SHMAP_VERIFY_DONE'
