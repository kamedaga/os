#!/usr/bin/env sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
scheduling_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
cd "$scheduling_dir"

jobs=${COQ_JOBS:-$(nproc)}
makefile=${COQ_MAKEFILE:-Makefile.coq}

case "${1:-build}" in
  build)
    if [ "$#" -gt 0 ]; then
      shift
    fi
    coq_makefile \
      -Q spec Pacha.Scheduling \
      -Q proof Pacha.Scheduling.Proof \
      -Q proof/generated Pacha.Scheduling.Clight \
      spec/ProtocolModel.v \
      spec/EevdfModel.v \
      spec/EevdfInvariants.v \
      spec/EevdfTransitions.v \
      spec/EevdfCharge.v \
      spec/EevdfPick.v \
      spec/EevdfPreservation.v \
      spec/SchedRuntimeModel.v \
      spec/SchedRuntimeSpec.v \
      spec/KernelSchedModel.v \
      spec/KernelSchedInvariants.v \
      spec/KernelSchedSpec.v \
      spec/KernelSchedPreservation.v \
      spec/KernelSchedTestVectors.v \
      proof/SchedRuntimeVstSpec.v \
      proof/generated/PachaEevdfClight.v \
      proof/generated/PachaSchedClight.v \
      proof/PachaEevdfVst.v \
      proof/PachaSchedVst.v \
      -o "$makefile"
    exec make -f "$makefile" -j"$jobs" "$@"
    ;;
  clean)
    if [ -f "$makefile" ]; then
      make -f "$makefile" clean
    fi
    find . proof spec \
      \( -name '*.vo' -o -name '*.vos' -o -name '*.vok' \
         -o -name '*.glob' -o -name '.*.aux' \
         -o -name '.lia.cache' -o -name '.nia.cache' \
         -o -name '.Makefile.coq.d' \) \
      -delete
    rm -f "$makefile" "$makefile.conf" \
      proof/Makefile.coq proof/Makefile.coq.conf \
      spec/Makefile.coq spec/Makefile.coq.conf
    ;;
  *)
    echo "usage: $0 [build|clean]" >&2
    exit 2
    ;;
esac
