#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
ulimit -c 0
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [[ -z "${IN_NIX_SHELL:-}" ]]; then
    exec nix develop "$root" --command bash "$0" "$@"
fi
python3 - "$root" "$@" <<'PY'
import hashlib
import importlib.util
from pathlib import Path
import signal
import subprocess
import sys
from types import SimpleNamespace
root = Path(sys.argv[1])
if len(sys.argv) not in (2, 4) or (len(sys.argv) == 4 and sys.argv[2] != '--before-source'):
    raise SystemExit('usage: run-kobox2-wifi-reconnect-unit.sh [--before-source ORIGINAL_SERVICE_C]')
source = root / 'kobox2/linux-sandbox'
spec = importlib.util.spec_from_file_location('task', source / 'kobox/task/build_task_smp.py')
task = importlib.util.module_from_spec(spec); spec.loader.exec_module(task)
out = root / '.artifacts/tests/kobox2-wifi-reconnect-unit'

def build(service_source, name):
    directory = out / name
    (directory / '.metadata').mkdir(parents=True, exist_ok=True)
    args = SimpleNamespace(source_tree=source, output_dir=directory, cc='clang-18',
        protocol_include=root / 'kobox2/protocol/generated/include',
        architecture_include=source / 'kobox/task/include',
        provider_build_dir=root / '.artifacts/kobox2-wifi-canonical',
        extra_include_dirs=[source / 'kobox/boot/include', source / 'kobox/boot',
                            root / 'kobox2/protocol/include'],
        extra_cflags=['-DKOBOX_BOOT_RUNTIME=1', '-DKOBOX_BOOT_WIFI=1', '-DKOBOX_BOOT_GPU=0'])
    obj = task.compile_support(args, str(service_source))
    binary = directory / 'reconnect-unit'
    subprocess.run(['/usr/bin/clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-no-pie',
        '-I', str(source / 'kobox'), str(root / 'tests/kobox2_wifi_reconnect_unit.c'),
        str(obj), '-o', str(binary)], check=True)
    return binary

subprocess.run([str(build(Path('kobox/boot/wifi_service.c'), 'fixed'))], check=True)
if len(sys.argv) == 4:
    before = Path(sys.argv[3]).resolve()
    if not before.is_file(): raise SystemExit('baseline source missing')
    result = subprocess.run([str(build(before, 'before'))], capture_output=True, text=True)
    (out / 'before-result.log').write_text(result.stdout + result.stderr)
    # A compile/link failure or unrelated crash is not evidence of regression.
    if result.returncode != -signal.SIGABRT or 'sequence=FR disconnected=1\n' not in result.stdout:
        raise SystemExit('baseline did not fail at the disconnected-latch regression')
    print('WiFi reconnect BEFORE: original FR leaves explicit disconnect latched; regression fails as expected PASS')
    (out / 'before-source.sha256').write_text(hashlib.sha256(before.read_bytes()).hexdigest() + '\n')
PY
