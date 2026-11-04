#!/usr/bin/env python3
from pathlib import Path
import re
root = Path(__file__).resolve().parents[1]
zig = {"LOG": 1, "KERNEL_LOG_READ": 2}
for path in (root/'kernel/abi').glob('*_abi.zig'):
    for name, value in re.findall(r'pub const syscall_(\w+): u64 = (\d+);', path.read_text()):
        if name.endswith(("_first", "_last")): continue
        zig[name.upper()] = int(value)
assert sorted(zig.values()) == list(range(1, 115)), sorted(zig.values())
for file in ['userland/libpacha/include/pacha/abi.h', 'musl/pachaos/include/pachaos/abi.h', 'musl/pachaos/syscall/native_raw.h']:
    source = (root/file).read_text()
    seen = set()
    for cname, number in re.findall(r'(PACHA\w*SYSCALL_\w+)\s*(?:=\s*)?(\d+)\b', source):
        prefix, operation = cname.split('SYSCALL_', 1)
        namespace = prefix.removeprefix('PACHAOS_').removeprefix('PACHA_').strip('_')
        if namespace in ['FD', 'RUNTIME', 'VM', '']: name = operation
        else: name = namespace + '_' + operation
        if name not in zig and namespace + "_" + operation in zig: name = namespace + "_" + operation
        if name not in zig: continue
        assert int(number) == zig[name], (file, cname, number, zig[name])
        seen.add(zig[name])
    required = set(range(64, 115 if file.startswith("userland/") else 92))
    assert required <= seen, (file, sorted(required - seen))
print('VMO discard ABI: Zig 1..114 contiguous; defined C/musl/raw bindings agree')
