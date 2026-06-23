#!/usr/bin/env sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root_dir=$(CDPATH= cd -- "$script_dir/../../.." && pwd)
out_dir="$script_dir/generated"

mkdir -p "$out_dir"

clightgen -std=c11 -normalize -U_FORTIFY_SOURCE \
  -I"$root_dir/verified/scheduling/include" \
  -o "$out_dir/PachaEevdfClight.v" \
  "$root_dir/verified/scheduling/src/pacha_eevdf.c"

clightgen -std=c11 -normalize -U_FORTIFY_SOURCE \
  -I"$root_dir/verified/scheduling/include" \
  -o "$out_dir/PachaSchedClight.v" \
  "$root_dir/verified/scheduling/src/pacha_sched.c"
