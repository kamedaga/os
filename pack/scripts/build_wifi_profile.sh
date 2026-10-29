#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Canonical configuration, vmlinux/modules and inventories only.
set -euo pipefail

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
configure_only=0
if [[ $# == 1 && "$1" == --configure-only ]]; then
    configure_only=1
elif [[ $# != 0 ]]; then
    echo "usage: $0 [--configure-only]" >&2
    exit 2
fi
if [[ -z "${IN_NIX_SHELL:-}" ]]; then
    exec nix develop "$repo_root" --command bash "$0" "$@"
fi
source "$repo_root/pack/scripts/lib/jobs.sh"
source_tree="$repo_root/kobox2/linux-sandbox"
canonical="$repo_root/.artifacts/kobox2-wifi-canonical"
output="$repo_root/.artifacts/kobox2-wifi-profile"
profile="$source_tree/kobox/manifest/profiles/wifi.json"
fragment="$source_tree/kobox/manifest/profiles/wifi.config"
jobs="${JOBS:-${PACGO_BUILD_JOBS:-4}}"
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo 'JOBS must be positive' >&2; exit 2; }

if [[ -n "${CAPOS_LINUX_LLVM18_BIN:-}" ]]; then
    export PATH="$CAPOS_LINUX_LLVM18_BIN:$PATH"
fi
if [[ -n "${CAPOS_LIBELF_INCLUDE:-}" && -n "${CAPOS_LIBELF_LIB:-}" ]]; then
    export HOSTCFLAGS="${HOSTCFLAGS:-} -isystem${CAPOS_LIBELF_INCLUDE}"
    export LIBRARY_PATH="${CAPOS_LIBELF_LIB}${LIBRARY_PATH:+:$LIBRARY_PATH}"
fi
expected_compiler=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["toolchain"]["compiler_version"])' "$profile")
compiler_version=$(clang-18 --version | head -1)
if [[ "$compiler_version" != *"$expected_compiler"* ]]; then
    echo "Wi-Fi profile requires clang-18 $expected_compiler" >&2
    exit 1
fi
mkdir -p "$canonical" "$output"
kconfig=$(python3 "$source_tree/kobox/boot/prepare_kconfig.py" \
    --source-tree "$source_tree" --build-dir "$canonical")
expected_digest=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["config_sha256"])' "$profile")
current_digest=""
if [[ -f "$canonical/.config" ]]; then
    current_digest=$(sha256sum "$canonical/.config" | cut -d' ' -f1)
fi
if [[ "$configure_only" == 1 || "$current_digest" != "$expected_digest" ||
      "$source_tree/kobox/task/config" -nt "$canonical/.config" ||
      "$source_tree/kobox/boot/config" -nt "$canonical/.config" ||
      "$fragment" -nt "$canonical/.config" ]]; then
    make -C "$source_tree" O="$canonical" ARCH=x86 LLVM=-18 KBUILD_KCONFIG="$kconfig" tinyconfig
    "$source_tree/scripts/kconfig/merge_config.sh" -m -r -O "$canonical" \
        "$canonical/.config" "$source_tree/kobox/task/config" \
        "$source_tree/kobox/boot/config" "$fragment"
    make -C "$source_tree" O="$canonical" ARCH=x86 LLVM=-18 KBUILD_KCONFIG="$kconfig" olddefconfig
fi
python3 - "$profile" "$canonical/.config" "$configure_only" <<'PY'
import hashlib, json, sys
from pathlib import Path
profile = json.loads(Path(sys.argv[1]).read_text())
text = Path(sys.argv[2]).read_text()
config = {}
for line in text.splitlines():
    if line.startswith('CONFIG_') and '=' in line:
        key, value = line.split('=', 1)
        config[key] = value
    elif line.startswith('# CONFIG_') and line.endswith(' is not set'):
        config[line[2:-11]] = 'n'
for key, expected in profile['required_config'].items():
    if config.get(key, 'n') != expected:
        raise SystemExit(f'{key}: expected {expected}, got {config.get(key, "n")}')
digest = hashlib.sha256(text.encode()).hexdigest()
print(f'Wi-Fi canonical config sha256={digest}', flush=True)
if sys.argv[3] != '1' and digest != profile['config_sha256']:
    raise SystemExit('Wi-Fi canonical config differs from its pinned profile')
PY
if [[ "$configure_only" == 1 ]]; then
    exit 0
fi
pacgo_make "$jobs" -C "$source_tree" O="$canonical" ARCH=x86 LLVM=-18 \
    KBUILD_KCONFIG="$kconfig" vmlinux modules
python3 "$source_tree/kobox/manifest/generate_boot_core_inventory.py" \
    --source-tree "$source_tree" --build-dir "$canonical" --profile "$profile" \
    --nm llvm-nm-18 --readelf llvm-readelf-18 --output "$output/boot-core-inventory.json"
python3 "$source_tree/kobox/manifest/generate_closure_inventory.py" \
    --source-tree "$source_tree" --build-dir "$canonical" --profile "$profile" \
    --nm llvm-nm-18 --output "$output/closure-inventory.json"
python3 - "$canonical" "$output/closure-inventory.json" <<'PY'
import json, subprocess, sys
from pathlib import Path
canonical = Path(sys.argv[1])
inventory = json.loads(Path(sys.argv[2]).read_text())
modules = set(inventory['load_order'])
required = {
    'drivers/base/firmware_loader/firmware_class.ko',
    'net/rfkill/rfkill.ko', 'net/wireless/cfg80211.ko',
    'net/mac80211/mac80211.ko',
    'drivers/net/wireless/intel/iwlwifi/iwlwifi.ko',
    'drivers/net/wireless/intel/iwlwifi/mvm/iwlmvm.ko',
}
if not required <= modules:
    raise SystemExit(f'Wi-Fi closure is missing {sorted(required - modules)}')
if any('hwsim' in name or name.endswith(('/virtio_net.ko', '/r8169.ko')) for name in modules):
    raise SystemExit('Non-production Wi-Fi or Ethernet module entered the closure')
# NET=y always builds netlink and generic netlink; there are no separate
# CONFIG_NETLINK/CONFIG_GENERIC_NETLINK switches. Audit real link inputs.
members = subprocess.check_output(['llvm-ar-18', 't', str(canonical / 'vmlinux.a')], text=True).splitlines()
for required_object in ('net/netlink/af_netlink.o', 'net/netlink/genetlink.o',
                        'net/core/rtnetlink.o', 'net/packet/af_packet.o'):
    if not any(member.endswith('/' + required_object) or member == required_object for member in members):
        raise SystemExit(f'Wi-Fi core is missing {required_object}')
print(f'Wi-Fi closure modules={len(modules)}; netlink/genl/rtnetlink/packet core inputs verified')
PY
echo "Wi-Fi canonical build and inventories passed: $output"
