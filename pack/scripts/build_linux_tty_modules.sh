#!/usr/bin/env bash
set -euo pipefail

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out="${1:-.artifacts/kobox-linux-tty}"

kdir=$(bash "$repo_root/pack/scripts/fetch_arch_linux_6_8_headers.sh" | tail -n 1)
KDIR="$kdir" bash "$repo_root/pack/scripts/build_linux_tty_ko.sh" "$out" >/dev/null
