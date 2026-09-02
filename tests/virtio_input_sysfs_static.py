#!/usr/bin/env python3
"""Static regression checks for the generated virtio-input sysfs tree."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SYSFS = ROOT / ".artifacts/userland-fixtures/virtio-input-sysfs"


def text(path: Path) -> str:
    return path.read_text(encoding="ascii").strip()


def bitmap(path: Path) -> int:
    value = 0
    for word in text(path).split():
        value = (value << 64) | int(word, 16)
    return value


def properties(path: Path) -> set[str]:
    return {line for line in text(path).splitlines() if line.startswith("ID_INPUT")}


def main() -> None:
    keyboard = SYSFS / "sys/devices/virtual/input/input0"
    mouse = SYSFS / "sys/devices/virtual/input/input1"
    assert text(keyboard / "name") == "QEMU Virtio Keyboard"
    assert text(keyboard / "phys") == "virtio0/input0"
    assert text(keyboard / "id/bustype") == "0006"
    assert text(keyboard / "id/vendor") == "0627"
    assert text(keyboard / "id/product") == "0001"
    assert text(keyboard / "id/version") == "0001"
    assert text(keyboard / "capabilities/ev") == "120003"
    assert text(keyboard / "capabilities/key") == (
        "400000007 ff803078f800dfff febeffff7bcfffff fffffffffffffffe"
    )
    assert text(keyboard / "capabilities/led") == "7"
    keyboard_keys = bitmap(keyboard / "capabilities/key")
    assert keyboard_keys & 0xFFFFFFFE == 0xFFFFFFFE
    assert properties(keyboard / "event0/uevent") == {
        "ID_INPUT=1",
        "ID_INPUT_KEY=1",
        "ID_INPUT_KEYBOARD=1",
    }
    assert "PRODUCT=6/627/1/1" in text(keyboard / "uevent").splitlines()

    assert text(mouse / "name") == "QEMU Virtio Mouse"
    assert text(mouse / "phys") == "virtio1/input0"
    assert text(mouse / "id/bustype") == "0006"
    assert text(mouse / "id/vendor") == "0627"
    assert text(mouse / "id/product") == "0002"
    assert text(mouse / "id/version") == "0002"
    assert text(mouse / "capabilities/ev") == "7"
    assert text(mouse / "capabilities/key") == "30400 1f0000 0 0 0 0"
    assert text(mouse / "capabilities/rel") == "103"
    mouse_events = bitmap(mouse / "capabilities/ev")
    mouse_keys = bitmap(mouse / "capabilities/key")
    mouse_relative = bitmap(mouse / "capabilities/rel")
    assert mouse_events & (1 << 1) and mouse_events & (1 << 2)
    assert mouse_keys & (1 << 0x110)
    assert mouse_relative & 0x3 == 0x3
    assert properties(mouse / "event1/uevent") == {
        "ID_INPUT=1",
        "ID_INPUT_MOUSE=1",
    }
    assert "PRODUCT=6/627/2/2" in text(mouse / "uevent").splitlines()
    print("virtio input sysfs static checks: OK")


if __name__ == "__main__":
    main()
