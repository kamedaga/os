#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [[ -z "${IN_NIX_SHELL:-}" ]]; then
    exec nix develop "$root" --command bash "$0" "$@"
fi
python3 - "$root" <<'PY'
import importlib.util
from pathlib import Path
import subprocess, sys
from types import SimpleNamespace
root=Path(sys.argv[1]); source=root/'kobox2/linux-sandbox'
spec=importlib.util.spec_from_file_location('task',source/'kobox/task/build_task_smp.py')
task=importlib.util.module_from_spec(spec); spec.loader.exec_module(task)
out=root/'.artifacts/tests/kobox2-wifi-service-unit'; (out/'.metadata').mkdir(parents=True,exist_ok=True)
args=SimpleNamespace(source_tree=source,output_dir=out,cc='clang-18',
    protocol_include=root/'kobox2/protocol/generated/include',
    architecture_include=source/'kobox/task/include',provider_build_dir=root/'.artifacts/kobox2-wifi-canonical',
    extra_include_dirs=[source/'kobox/boot/include',root/'kobox2/protocol/include'],
    extra_cflags=['-DKOBOX_BOOT_RUNTIME=1','-DKOBOX_BOOT_WIFI=1','-DKOBOX_BOOT_GPU=0'])
obj=task.compile_support(args,'kobox/boot/wifi_service.c')
binary=out/'service-unit'
subprocess.run(['/usr/bin/clang','-std=c11','-Wall','-Wextra','-Werror','-no-pie',
    '-I',str(source/'kobox'),str(root/'tests/kobox2_wifi_service_unit.c'),str(obj),'-o',str(binary)],check=True)
subprocess.run([str(binary)],check=True)
PY
