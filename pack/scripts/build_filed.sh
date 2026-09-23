#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$repo_root"

bash pack/scripts/download_zydis.sh >/dev/null
exec bash pack/scripts/build_cmake_app.sh userland/filed .artifacts/cmake/filed filed
