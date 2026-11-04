#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="$repo_root/.artifacts/perf/helpers"
mkdir -p "$out/classes"
cc -O2 -Wall -Wextra "$repo_root/tools/perf/x11_probe.c" -o "$out/x11_probe" -lX11 -lXtst
javac --release 17 -d "$out/classes" "$repo_root/tools/perf/PachaPerfAgent.java"
printf 'Premain-Class: PachaPerfAgent\nCan-Redefine-Classes: false\n\n' > "$out/manifest.mf"
jar cfm "$out/pacha-perf-agent.jar" "$out/manifest.mf" -C "$out/classes" .
printf '%s\n' "$out/x11_probe" "$out/pacha-perf-agent.jar"
