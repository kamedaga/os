#!/usr/bin/env bash
set -euo pipefail

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
C_INCLUDE_PATH="$repo_root/userland/libaccount/include" \
    bash "$repo_root/pack/scripts/build_lpr_pthread_smoke.sh" \
    .artifacts/userland-fixtures/lpr_accounts \
    userland/fixtures/src/wsl_musl/lpr_accounts.c
