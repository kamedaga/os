#!/usr/bin/env bash
set -euo pipefail

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
src="${repo_root}/third_party/linux-6.8-tty"
out_arg="${1:-${repo_root}/.artifacts/kobox-linux-tty}"
mkdir -p "${out_arg}"
out=$(CDPATH= cd -- "${out_arg}" && pwd)
work="${out}/src"
kdir="${KDIR:-/lib/modules/$(uname -r)/build}"
tool_include="${repo_root}/.artifacts/tools/include"
tool_lib="${repo_root}/.artifacts/tools/lib"
lock="${out}/.build.lock"
module_symvers_backup=""

exec 9>"${lock}"
flock 9

if [ ! -e "${kdir}/Makefile" ]; then
    echo "linux tty ko build skipped: kernel build tree not found: ${kdir}" >&2
    echo "set KDIR=/path/to/linux-6.8/build to build linux_tty_core.ko" >&2
    exit 77
fi

rm -rf "${work}"
mkdir -p "${work}"
rm -f "${out}"/*.ko
cp -a "${src}/." "${work}/"

kernel_release=$(cat "${kdir}/include/config/kernel.release" 2>/dev/null || true)
if [ -f "${kdir}/Module.symvers" ] && [ "${kernel_release}" = "6.8.0-arch1-1" ]; then
    module_symvers_backup="${kdir}/Module.symvers.kobox-backup"
    mv "${kdir}/Module.symvers" "${module_symvers_backup}"
fi

cleanup_module_symvers() {
    if [ -n "${module_symvers_backup}" ] && [ -f "${module_symvers_backup}" ]; then
        mv "${module_symvers_backup}" "${kdir}/Module.symvers"
    fi
}
trap cleanup_module_symvers EXIT

env \
    "PATH=${repo_root}/.artifacts/tools/bin:${PATH}" \
    "PKG_CONFIG_PATH=${tool_lib}/pkgconfig:${PKG_CONFIG_PATH:-}" \
    "LD_LIBRARY_PATH=${tool_lib}:${LD_LIBRARY_PATH:-}" \
    "HOSTCFLAGS=-I${tool_include}" \
    "HOSTLDFLAGS=-L${tool_lib} -Wl,-rpath,${tool_lib}" \
    "KBUILD_MODPOST_WARN=1" \
    make -C "${kdir}" M="${work}" PAHOLE=true modules

for module in \
    linux_tty_core.ko \
    linux_tty_n_null.ko \
    linux_virtio.ko \
    linux_virtio_ring.ko \
    linux_virtio_console.ko; do
    test -f "${work}/${module}"
    cp -f "${work}/${module}" "${out}/${module}"
    objcopy --strip-debug --remove-section=.BTF --remove-section=.BTF.ext "${out}/${module}"
done

"${repo_root}/pack/scripts/fetch_arch_linux_6_8_virtio_ko.sh" "${out}" --skip-console >/dev/null

printf '%s\n' "${out}"
