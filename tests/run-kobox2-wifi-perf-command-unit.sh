#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .artifacts/tests/kobox2-wifi-perf-command
clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined tests/kobox2_wifi_perf_command_unit.c -o .artifacts/tests/kobox2-wifi-perf-command/unit
.artifacts/tests/kobox2-wifi-perf-command/unit
