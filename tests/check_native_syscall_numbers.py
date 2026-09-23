#!/usr/bin/env python3
"""Check every handwritten native syscall number against the kernel ABI."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
kernel = {"log": 1}
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
    "musl/upstream/arch/pachaos/syscall_arch.h",
    "musl/pachaos/smoke/libc_vfs_exec.c",
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
