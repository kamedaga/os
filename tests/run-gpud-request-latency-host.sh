#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/gpud-request-latency
mkdir -p "$out"
clang -D_GNU_SOURCE -std=c11 -O2 -g -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-variable -Wno-macro-redefined -pthread -ffunction-sections -fdata-sections -Wl,--gc-sections -Ikobox2/include -Ikobox2/protocol/include -Ikobox2/protocol/generated/include -Iuserland/gpud/include -Iuserland/libipc/include -Iuserland/libpacha/include tests/gpud_request_latency_host.c userland/gpud/ready.c userland/gpud/drm_files.c -o "$out/roundtrip"
GPUD_BENCH_PIN=${GPUD_BENCH_PIN:-1} python3 - "$out/roundtrip" <<'PYCODE'
import os,sys
if os.environ['GPUD_BENCH_PIN'] != '0':
 cpu=min(os.sched_getaffinity(0));os.sched_setaffinity(0,{cpu});print('Host affinity CPU',cpu,flush=True)
os.execv(sys.argv[1],[sys.argv[1]])
PYCODE
