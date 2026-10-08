#!/usr/bin/env python3
"""Check every handwritten native syscall number against the kernel ABI."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
kernel = {"log": 1, "kernel_log_read": 2}
for group in ("process", "runtime", "fd", "vm", "ipc", "capsule"):
    source = (root / f"kernel/abi/{group}_abi.zig").read_text()
    for name, number in re.findall(r"pub const syscall_(\w+): u64 = (\d+);", source):
        if name.endswith(("_first", "_last", "_count")):
            continue
        assert name not in kernel, name
        kernel[name] = int(number)
assert sorted(kernel.values()) == list(range(1, len(kernel) + 1)), kernel

paths = (
    "userland/libpacha/include/pacha/abi.h",
    "musl/pachaos/include/pachaos/abi.h",
    "musl/pachaos/syscall/native_raw.h",
)
for path in paths:
    source = (root / path).read_text()
    pairs = re.findall(
        r"(?:#define\s+)?(PACHA(?:OS|_\w+)?)_SYSCALL_(\w+)\s+(?:=\s*)?(\d+)\b",
        source,
    )
    assert pairs, path
    seen = set()
    for prefix, name, number in pairs:
        if prefix in ("PACHA", "PACHAOS") and (name == "OK" or name.startswith("ERR_")):
            continue
        key = name.lower()
        if key not in kernel and prefix.startswith("PACHA_"):
            key = prefix.removeprefix("PACHA_").lower() + "_" + key
        assert key in kernel, (path, name)
        assert int(number) == kernel[key], (path, name, number, kernel[key])
        seen.add(key)
    if "smoke/" not in path:
        assert "yield" in seen, path
    if path.startswith("userland/"):
        # LOG is owned by the separate trace wrapper, not this header.
        assert seen == set(kernel) - {"log"}, (path, set(kernel) - seen)
    print(f"{path}: {len(seen)} numbers agree")
print(f"native syscall ABI: {len(kernel)} contiguous entries PASS")

# A synchronized header does not catch literal numbers hidden in native glue.
for path in (root / 'musl/pachaos', root / 'userland', root / 'tests'):
    for source in path.rglob('*'):
        if source.suffix not in ('.c', '.h', '.S'):
            continue
        calls = re.findall(r'(?:pacha_syscall[0-6]|__pachaos_raw[0-6]|native)\(\s*(\d+)\b', source.read_text())
        assert all(int(n) < 29 for n in calls), (source, calls)
print('native source callers: no renumberable syscall literals PASS')
