#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
out="$root/.artifacts/lpr-tests"
mkdir -p "$out"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -O2 -pthread -I"$root/musl/pachaos/include" "$root/userland/personality/linux/tests/futex_wake_only_test.c" -o "$out/futex-wake-only"
"$out/futex-wake-only"
