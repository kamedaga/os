#!/bin/sh
set -eu

iterations="${1:-256}"
trials="${2:-5}"

echo "LPR_FUTEX_PINGPONG_BEGIN iterations=$iterations trials=$trials"
/cmd/lpr_futex_pingpong_bench.elf "$iterations" "$trials"
echo LPR_FUTEX_PINGPONG_DONE
