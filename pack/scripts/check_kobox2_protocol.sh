#!/usr/bin/env bash
set -euo pipefail

# The kobox2 protocol layouts are generated from schema/ and committed under
# generated/. Builds compile the committed headers, so this recipe proves they
# still match the schema. The stamp is constant: passing again must not make
# any consumer rebuild.
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
stamp=${1:?usage: $0 <stamp>}
case "$stamp" in /*) ;; *) stamp="$repo_root/$stamp" ;; esac

python3 "$repo_root/kobox2/tools/generate_protocol.py" --check
mkdir -p "$(dirname "$stamp")"
printf 'PASS\n' > "$stamp.tmp"
mv "$stamp.tmp" "$stamp"
