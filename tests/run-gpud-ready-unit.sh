#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/gpud-ready
mkdir -p "$out"
flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer -Iuserland/libipc/include -Iuserland/libpacha/include)
clang "${flags[@]}" tests/gpud_ready_host.c userland/gpud/ready.c -o "$out/host-sanitized"
UBSAN_OPTIONS=halt_on_error=1 "$out/host-sanitized"
clang "${flags[@]}" tests/gpud_index_unit.c -o "$out/index-sanitized"
UBSAN_OPTIONS=halt_on_error=1 "$out/index-sanitized"
clang "${flags[@]}" -D_GNU_SOURCE -Wno-unused-function -Wno-unused-variable -Wno-macro-redefined -ffunction-sections -fdata-sections -Wl,--gc-sections -Ikobox2/include -Ikobox2/protocol/include -Ikobox2/protocol/generated/include -Iuserland/gpud/include tests/gpud_push_service_unit.c userland/gpud/ready.c userland/gpud/drm_files.c -o "$out/push-service"
UBSAN_OPTIONS=halt_on_error=1 "$out/push-service"
