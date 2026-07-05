#!/usr/bin/env bash
set -euo pipefail

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
archive="${repo_root}/.artifacts/linux-6.8.tar.gz"
src="${repo_root}/.artifacts/linux-6.8-full"
build="${1:-${repo_root}/.artifacts/linux-6.8-build}"
lock="${repo_root}/.artifacts/linux-6.8-build.lock"

mkdir -p "${repo_root}/.artifacts"
exec 9>"${lock}"
flock 9

if [ ! -f "${archive}" ]; then
    curl -L --fail --retry 3 \
        "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.8.tar.gz" \
        -o "${archive}"
fi

if [ ! -f "${src}/Makefile" ]; then
    rm -rf "${src}"
    mkdir -p "${src}.tmp"
    tar -xzf "${archive}" -C "${src}.tmp" --strip-components=1
    mv "${src}.tmp" "${src}"
fi

tool_path="${repo_root}/.artifacts/tools/bin:${PATH}"
tool_include="${repo_root}/.artifacts/tools/include"
tool_lib="${repo_root}/.artifacts/tools/lib"
tool_pkgconfig="${tool_lib}/pkgconfig"

missing=0
for tool in make gcc bc flex bison; do
    if ! command -v "${tool}" >/dev/null 2>&1; then
        if ! PATH="${tool_path}" command -v "${tool}" >/dev/null 2>&1; then
            echo "linux 6.8 prepare missing tool: ${tool}" >&2
            missing=1
        fi
    fi
done
if [ "${missing}" -ne 0 ]; then
    echo "install the missing tools or provide a prepared KDIR to build linux_tty_core.ko" >&2
    exit 77
fi

mkdir -p "${build}"
make_env=(
    "PATH=${tool_path}"
    "PKG_CONFIG_PATH=${tool_pkgconfig}:${PKG_CONFIG_PATH:-}"
    "LD_LIBRARY_PATH=${tool_lib}:${LD_LIBRARY_PATH:-}"
    "HOSTCFLAGS=-I${tool_include}"
    "HOSTLDFLAGS=-L${tool_lib} -Wl,-rpath,${tool_lib}"
)

env "${make_env[@]}" make -C "${src}" O="${build}" defconfig
PATH="${tool_path}" "${src}/scripts/config" --file "${build}/.config" \
    --enable MODULES \
    --enable MODULE_UNLOAD \
    --disable X86_KERNEL_IBT \
    --disable RETPOLINE \
    --disable RETHUNK \
    --disable SLS \
    --disable UNWINDER_ORC \
    --enable UNWINDER_FRAME_POINTER \
    --disable STACK_VALIDATION \
    --disable NOINSTR_VALIDATION \
    --disable DEBUG_ENTRY \
    --disable JUMP_LABEL \
    --disable FTRACE \
    --disable DYNAMIC_FTRACE
env "${make_env[@]}" make -C "${src}" O="${build}" olddefconfig
env "${make_env[@]}" make -C "${src}" O="${build}" modules_prepare

printf '%s\n' "${build}"
