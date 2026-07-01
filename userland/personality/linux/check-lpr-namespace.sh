#!/usr/bin/env bash
set -euo pipefail

image="${1:?usage: check-lpr-namespace.sh LPR_SO}"

readelf_bin="${READELF:-}"
if [[ -z "$readelf_bin" ]]; then
  if command -v llvm-readelf >/dev/null 2>&1; then
    readelf_bin="llvm-readelf"
  else
    readelf_bin="readelf"
  fi
fi

"$readelf_bin" --dyn-syms "$image" | awk '
  /^[[:space:]]*[0-9]+:/ {
    ndx = $7
    name = $8
    if (ndx != "UND") {
      if (name == "") name = "<anonymous>"
      printf("LPR namespace leak: dynsym name=%s ndx=%s\n", name, ndx) > "/dev/stderr"
      bad = 1
    }
  }
  END { exit bad }
'
