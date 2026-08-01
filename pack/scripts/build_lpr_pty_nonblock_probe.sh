#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
out="${1:-.artifacts/userland-fixtures/lpr_pty_nonblock_probe.elf}"
src="${repo_root}/userland/fixtures/linux/lpr_pty_nonblock_probe.c"
cc="${PACHAOS_LINUX_MUSL_CC:-/usr/bin/x86_64-linux-musl-gcc}"

if [[ ! -x "${cc}" ]]; then
  cc="x86_64-linux-musl-gcc"
fi

mkdir -p "$(dirname "${repo_root}/${out}")"

"${cc}" \
  -std=c11 \
  -Wall \
  -Wextra \
  -Werror \
  "${src}" \
  -Wl,--dynamic-linker=/lib/linux/ld-musl-x86_64.so.1 \
  -o "${repo_root}/${out}"
