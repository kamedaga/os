#!/usr/bin/env bash
# EXPECTED RED (2026-07-14): a forked child cannot create threads.
#
# Measured on main 1a41f06: the parent creates and joins a thread fine, but
# pthread_create inside a plain fork() child returns EAGAIN (11).  musl maps
# every clone failure to EAGAIN, so the underlying LPR/kernel errno is not
# visible from the guest; an mmap of a thread-stack-sized region inside the
# same child succeeds, which rules out address-space exhaustion and points at
# the clone/THREAD_CREATE path itself.
#
# This runner asserts the CORRECT behaviour (all three cases OK) and therefore
# fails today.  It is intentionally NOT part of the smoke battery until the
# defect is fixed.  Do not weaken the expectations to make it pass.
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

if [[ "${SKIP_SYNC:-0}" != "1" ]]; then
  .artifacts/bin/pacgo sync rootfs --force
  .artifacts/bin/pacgo sync bootfs
fi

pkill -9 qemu-system-x86 2>/dev/null || true
.artifacts/bin/pacgo qemu-test \
  --timeout 60s \
  --boot-marker '[termd] linux tty hvc open ready index=0 handle=2' \
  --send '/cmd/lpr_fork_pthread_probe.elf' \
  --expect 'FORK_PTHREAD_START' \
  --expect 'FORK_PTHREAD_PARENT=OK rc=0' \
  --expect 'FORK_PTHREAD_CHILD_PLAIN=OK rc=0' \
  --expect 'FORK_PTHREAD_CHILD_AFTER_KILL=OK rc=0' \
  --expect 'FORK_PTHREAD_DONE'
