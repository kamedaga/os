#!/usr/bin/env sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
spec_dir="$script_dir/spec"
cd "$spec_dir"

jobs=${COQ_JOBS:-$(nproc)}
makefile=${COQ_MAKEFILE:-Makefile.coq}

case "${1:-build}" in
  build)
    if [ "$#" -gt 0 ]; then
      shift
    fi
    coq_makefile -f _CoqProject -o "$makefile"
    exec make -f "$makefile" -j"$jobs" "$@"
    ;;
  clean)
    if [ -f "$makefile" ]; then
      make -f "$makefile" clean
    fi
    find . \
      \( -name '*.vo' -o -name '*.vos' -o -name '*.vok' \
         -o -name '*.glob' -o -name '.*.aux' \
         -o -name '.lia.cache' -o -name '.nia.cache' \) \
      -delete
    rm -f "$makefile" "$makefile.conf" .Makefile.coq.d
    ;;
  *)
    echo "usage: $0 [build|clean]" >&2
    exit 2
    ;;
esac
