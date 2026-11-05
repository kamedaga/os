#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
b=.artifacts/epiphany-emfile/klogudp-build
inc=(-Iuserland/libpacha/include -Iuserland/libipc/include -Iuserland/netd/include
 -Iuserland/filed/include -Iuserland/libfiled_client/include
 -Ikobox2/protocol/include -Ikobox2/protocol/generated/include)
cc -O2 -Wall -Wextra -Werror "${inc[@]}" userland/klogudp/tests/worker.c -o "$b/worker-test"
for mode in 0 1 2; do "$b/worker-test" "$mode"; done
cc -O2 -Wall -Wextra -Werror "${inc[@]}" userland/klogudp/tests/launch.c \
 userland/filed/src/exec_payload.c -o "$b/launch-test"
"$b/launch-test"
cc -O2 -mno-red-zone -ffunction-sections -fdata-sections -Wl,--gc-sections \
 -I"$b/bridge" "${inc[@]}" userland/klogudp/tests/bridge.c "$b/bridge/syscall.c" -o "$b/bridge-test"
"$b/bridge-test"
python3 - "$b" <<'PY'
import subprocess,sys,re
from pathlib import Path
b=Path(sys.argv[1])
for name in ['klogudp.elf','klogudp-start']:
    p=b/name
    elf=subprocess.check_output(['readelf','-lW',p],text=True)
    assert 'INTERP' not in elf
    assert not subprocess.check_output(['nm','-u',p],text=True).strip()
s=subprocess.check_output(['objdump','-d',b/'klogudp-start'],text=True)
for name in ['pacha_syscall1','pacha_syscall2','pacha_syscall3','pacha_syscall6','pacha_ipc_registers']:
    match=re.search(r'<'+name+r'>:\n(.*?)(?=\n\n)',s,re.S)
    assert match and 'syscall\n' not in match[1] and '<klogudp_gate>' in match[1],name
print('ELF: static, no interpreter/unresolved symbols; native bridge uses indirect gate')
PY
