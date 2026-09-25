#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo_root/.artifacts/tests/epiphany-mmap-probe"
mkdir -p "$out"
flags=(-O2 -Wall -Wextra -Werror)
musl-gcc "${flags[@]}" -shared -fPIC -DPAGE_MMAP_DETAIL=1 \
    "$repo_root/tests/epiphany_load_calls.c" -ldl -pthread -o "$out/probe.so"
musl-gcc "${flags[@]}" -shared -fPIC -DDELAY_PROVIDER=1 \
    "$repo_root/tests/epiphany_mmap_probe_unit.c" -o "$out/delay.so"
musl-gcc "${flags[@]}" "$repo_root/tests/epiphany_mmap_probe_unit.c" -o "$out/unit"
LD_PRELOAD="$out/probe.so:$out/delay.so" "$out/unit" 2>"$out/probe.log"
test "$(rg -c '^PAGE_MMAP ' "$out/probe.log")" = 48
rg -q 'len=4096 prot=3 flags=22 fd=-1 offset=0 .*errno=0 .*path=-' "$out/probe.log"
rg -q 'len=0 .*errno=22 ' "$out/probe.log"
