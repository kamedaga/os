#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
python3 - "$root" <<'PY'
import importlib.util,subprocess,sys
from pathlib import Path
from types import SimpleNamespace
root=Path(sys.argv[1]);source=root/'kobox2/linux-sandbox';base=root/'.artifacts/tests/kobox2-net-rx-unit'
spec=importlib.util.spec_from_file_location('task',source/'kobox/task/build_task_smp.py');task=importlib.util.module_from_spec(spec);spec.loader.exec_module(task)
for control in (0,1):
    out=base/str(control);(out/'.metadata').mkdir(parents=True,exist_ok=True)
    args=SimpleNamespace(source_tree=source,output_dir=out,cc='/usr/bin/clang-18',protocol_include=root/'kobox2/protocol/generated/include',architecture_include=source/'kobox/task/include',provider_build_dir=root/'.artifacts/kobox2-wifi-canonical',extra_include_dirs=[source/'kobox/boot/include',root/'kobox2/protocol/include'],extra_cflags=['-DKOBOX_BOOT_RUNTIME=1','-DKOBOX_BOOT_WIFI=0','-DKOBOX_BOOT_NET=1','-DKOBOX_BOOT_GPU=0',f'-DKOBOX_NET_RX_CONTROL={control}'])
    obj=task.compile_support(args,str(root/'tests/kobox2_net_rx_fixture.c'));binary=out/'net-rx-unit'
    subprocess.run(['/usr/bin/clang','-std=c11','-Wall','-Wextra','-Werror','-no-pie','-Wl,--gc-sections',f'-DKOBOX_NET_RX_CONTROL={control}',str(root/'tests/kobox2_net_rx_unit.c'),str(obj),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
PY
