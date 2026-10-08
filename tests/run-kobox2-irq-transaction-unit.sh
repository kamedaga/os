#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$repo_root/.artifacts/tests/kobox2-irq-transaction"
mkdir -p "$out_dir"
"${CC:-cc}" -std=gnu11 -O2 -g -mgeneral-regs-only -Wall -Wextra -Werror \
    "$repo_root/kobox2/linux-sandbox/kobox/task/irq_transaction_test.c" \
    -o "$out_dir/irq-transaction-test"
"$out_dir/irq-transaction-test" | tee "$out_dir/result.log"
