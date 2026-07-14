#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
source_file="${repo_root}/userland/personality/linux/runtime/lpr_entry.S"

check_call_site() {
  local symbol="$1"
  local target="$2"
  awk -v symbol="$symbol" -v target="$target" '
    $0 ~ "^" symbol ":$" {
      in_function = 1
      next
    }
    in_function && $0 ~ "^[[:space:]]*\\.size[[:space:]]+" symbol "," {
      in_function = 0
    }
    !in_function { next }
    /^[[:space:]]*(#.*)?$/ { next }
    /^[[:space:]]*and[q]?[[:space:]]+\$-16,[[:space:]]*%rsp([[:space:]]*(#.*)?)?$/ {
      if (saw_alignment || saw_call) bad = 1
      saw_alignment = 1
      expect_call = 1
      next
    }
    expect_call {
      if ($0 ~ "^[[:space:]]*call[q]?[[:space:]]+" target "([[:space:]]*(#.*)?)?$") {
        saw_call = 1
      } else {
        bad = 1
      }
      expect_call = 0
      next
    }
    $0 ~ "^[[:space:]]*call[q]?[[:space:]]+" target "([[:space:]]*(#.*)?)?$" {
      bad = 1
    }
    END {
      if (!saw_alignment || !saw_call || bad) exit 1
    }
  ' "$source_file"
}

check_call_site lpr_async_signal_entry lpr_linux_async_signal_prepare
check_call_site lpr_async_signal_restorer lpr_linux_rt_sigreturn_body

echo "LPR_SIGNAL_ENTRY_ALIGNMENT=OK entry_call_rsp_mod16=0 restorer_call_rsp_mod16=0"
