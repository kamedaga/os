#!/usr/bin/env sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../.." && pwd)
artifacts_dir="$repo_root/.artifacts/verified-scheduling"
cc=${CC:-clang}

mkdir -p "$artifacts_dir"
cd "$repo_root"

cflags="-std=c11 -Wall -Wextra -Werror -Iverified/scheduling/include"

build_and_run() {
  name=$1
  shift
  "$cc" $cflags "$@" -o "$artifacts_dir/$name"
  "$artifacts_dir/$name"
}

build_and_run test_pacha_eevdf \
  verified/scheduling/src/pacha_eevdf.c \
  verified/scheduling/tests/test_pacha_eevdf.c

build_and_run test_pacha_eevdf_property \
  verified/scheduling/src/pacha_eevdf.c \
  verified/scheduling/tests/test_pacha_eevdf_property.c

build_and_run test_pacha_sched \
  verified/scheduling/src/pacha_eevdf.c \
  verified/scheduling/src/pacha_sched.c \
  verified/scheduling/tests/test_pacha_sched.c

build_and_run test_pacha_kernel_sched \
  verified/scheduling/src/pacha_eevdf.c \
  verified/scheduling/src/pacha_kernel_sched.c \
  verified/scheduling/tests/test_pacha_kernel_sched.c

build_and_run test_pacha_kernel_sched_property \
  verified/scheduling/src/pacha_eevdf.c \
  verified/scheduling/src/pacha_kernel_sched.c \
  verified/scheduling/tests/test_pacha_kernel_sched_property.c

coq_targets="
  spec/EevdfCharge.vo
  spec/EevdfPreservation.vo
  spec/KernelSchedModel.vo
  spec/KernelSchedInvariants.vo
  spec/KernelSchedSpec.vo
  spec/KernelSchedPreservation.vo
  spec/KernelSchedTestVectors.vo
"

if command -v coq_makefile >/dev/null 2>&1; then
  "$script_dir/proof/verify-coq.sh" build $coq_targets
elif command -v nix >/dev/null 2>&1; then
  nix develop -c env COQ_JOBS="${COQ_JOBS:-$(nproc)}" \
    "$script_dir/proof/verify-coq.sh" build $coq_targets
else
  echo "coq_makefile not found; run inside nix develop or install Coq/Rocq" >&2
  exit 127
fi
