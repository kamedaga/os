#!/usr/bin/env python3
"""Derive udev ID_INPUT_* properties from Linux input sysfs bitmaps.

The classification below follows systemd's udev-builtin-input_id.c.  Keeping
this as a bitmap consumer makes the generated sysfs metadata follow the device
capabilities instead of a desktop-session-specific device-name table.
"""

from __future__ import annotations

import argparse
from pathlib import Path


EV_KEY = 0x01
EV_REL = 0x02
EV_SW = 0x05

KEY_LEFTCTRL = 29
KEY_CAPSLOCK = 58
KEY_NUMLOCK = 69
KEY_INSERT = 110
KEY_MUTE = 113
KEY_CALC = 140
KEY_FILE = 144
KEY_MAIL = 155
KEY_PLAYPAUSE = 164
KEY_BRIGHTNESSDOWN = 224
KEY_OK = 0x160
KEY_ALS_TOGGLE = 0x230

BTN_MISC = 0x100
BTN_MOUSE = 0x110
BTN_JOYSTICK = 0x120
BTN_DIGI = 0x140
BTN_TOOL_PEN = 0x140
BTN_TOOL_FINGER = 0x145
BTN_TOUCH = 0x14A
BTN_STYLUS = 0x14B
BTN_DPAD_UP = 0x220
BTN_DPAD_RIGHT = 0x223
BTN_TRIGGER_HAPPY = 0x2C0
BTN_TRIGGER_HAPPY1 = 0x2C0
BTN_TRIGGER_HAPPY40 = 0x2E7

REL_HWHEEL = 0x06
REL_WHEEL = 0x08

ABS_X = 0x00
ABS_Y = 0x01
ABS_Z = 0x02
ABS_RX = 0x03
ABS_PRESSURE = 0x18
ABS_MT_SLOT = 0x2F
ABS_MT_POSITION_X = 0x35
ABS_MT_POSITION_Y = 0x36

INPUT_PROP_DIRECT = 0x01
INPUT_PROP_POINTING_STICK = 0x05
INPUT_PROP_ACCELEROMETER = 0x06


def read_bitmap(path: Path) -> int:
    """Parse Linux's most-significant-word-first sysfs bitmap format."""
    words = path.read_text(encoding="ascii").split()
    if not words:
        raise ValueError(f"empty input capability bitmap: {path}")
    value = 0
    for word in words:
        parsed = int(word, 16)
        if parsed < 0 or parsed >= 1 << 64:
            raise ValueError(f"invalid 64-bit input capability word {word!r}: {path}")
        value = (value << 64) | parsed
    return value


def bit(mask: int, number: int) -> bool:
    return bool(mask & (1 << number))


def any_bit(mask: int, start: int, end: int) -> bool:
    """Return whether any bit in the half-open [start, end) range is set."""
    if end <= start:
        return False
    return bool(mask & (((1 << (end - start)) - 1) << start))


def count_bits(mask: int, start: int, end: int) -> int:
    if end <= start:
        return 0
    return (mask & (((1 << (end - start)) - 1) << start)).bit_count()


def pointer_properties(
    bustype: int,
    ev: int,
    absolute: int,
    key: int,
    relative: int,
    properties: int,
) -> tuple[list[str], bool]:
    result: list[str] = []
    has_keys = bit(ev, EV_KEY)
    has_abs_coordinates = bit(absolute, ABS_X) and bit(absolute, ABS_Y)
    has_3d_coordinates = has_abs_coordinates and bit(absolute, ABS_Z)
    is_accelerometer = bit(properties, INPUT_PROP_ACCELEROMETER)
    if not has_keys and has_3d_coordinates:
        is_accelerometer = True
    if is_accelerometer:
        return ["ID_INPUT_ACCELEROMETER=1"], True

    is_pointing_stick = bit(properties, INPUT_PROP_POINTING_STICK)
    has_stylus = bit(key, BTN_STYLUS)
    has_pen = bit(key, BTN_TOOL_PEN)
    finger_but_no_pen = bit(key, BTN_TOOL_FINGER) and not has_pen
    has_mouse_button = any_bit(key, BTN_MOUSE, BTN_JOYSTICK)
    has_rel_coordinates = (
        bit(ev, EV_REL) and bit(relative, 0) and bit(relative, 1)
    )
    has_mt_coordinates = (
        bit(absolute, ABS_MT_POSITION_X) and bit(absolute, ABS_MT_POSITION_Y)
    )
    if has_mt_coordinates and bit(absolute, ABS_MT_SLOT) and bit(absolute, ABS_MT_SLOT - 1):
        has_mt_coordinates = False
    is_direct = bit(properties, INPUT_PROP_DIRECT)
    has_touch = bit(key, BTN_TOUCH)
    has_pad_buttons = bit(key, 0x100) and bit(key, 0x101) and not has_pen
    has_wheel = bit(ev, EV_REL) and (
        bit(relative, REL_WHEEL) or bit(relative, REL_HWHEEL)
    )

    joystick_buttons = 0
    if not bit(key, BTN_JOYSTICK - 1):
        joystick_buttons += count_bits(key, BTN_JOYSTICK, BTN_DIGI)
        joystick_buttons += count_bits(
            key, BTN_TRIGGER_HAPPY1, BTN_TRIGGER_HAPPY40 + 1
        )
        joystick_buttons += count_bits(key, BTN_DPAD_UP, BTN_DPAD_RIGHT + 1)
    joystick_axes = count_bits(absolute, ABS_RX, ABS_PRESSURE)

    is_abs_mouse = False
    is_touchpad = False
    is_touchscreen = False
    is_tablet = False
    is_tablet_pad = False
    is_joystick = False
    if has_abs_coordinates:
        if has_stylus or has_pen:
            is_tablet = True
        elif finger_but_no_pen and not is_direct:
            is_touchpad = True
        elif has_mouse_button:
            is_abs_mouse = True
        elif has_touch or is_direct:
            is_touchscreen = True
        elif joystick_buttons or joystick_axes:
            is_joystick = True
    elif joystick_buttons or joystick_axes:
        is_joystick = True

    if has_mt_coordinates:
        if has_stylus or has_pen:
            is_tablet = True
        elif finger_but_no_pen and not is_direct:
            is_touchpad = True
        elif has_touch or is_direct:
            is_touchscreen = True

    if is_tablet and has_pad_buttons:
        is_tablet_pad = True
    if has_pad_buttons and has_wheel and not has_rel_coordinates:
        is_tablet = True
        is_tablet_pad = True

    is_mouse = (
        not is_tablet
        and not is_touchpad
        and not is_joystick
        and has_mouse_button
        and (has_rel_coordinates or not has_abs_coordinates)
    )
    # BUS_I2C is 0x18. systemd additionally tags an I2C mouse as a
    # pointing stick, while retaining ID_INPUT_MOUSE.
    if is_mouse and bustype == 0x18:
        is_pointing_stick = True

    if is_joystick:
        well_known_keyboard_keys = (
            KEY_LEFTCTRL,
            KEY_CAPSLOCK,
            KEY_NUMLOCK,
            KEY_INSERT,
            KEY_MUTE,
            KEY_CALC,
            KEY_FILE,
            KEY_MAIL,
            KEY_PLAYPAUSE,
            KEY_BRIGHTNESSDOWN,
        )
        known_keys = sum(bit(key, code) for code in well_known_keyboard_keys)
        if known_keys >= 4 or joystick_buttons + joystick_axes < 2:
            is_joystick = False
        if has_wheel and has_pad_buttons:
            is_joystick = False

    if is_pointing_stick:
        result.append("ID_INPUT_POINTINGSTICK=1")
    if is_mouse or is_abs_mouse:
        result.append("ID_INPUT_MOUSE=1")
    if is_touchpad:
        result.append("ID_INPUT_TOUCHPAD=1")
    if is_touchscreen:
        result.append("ID_INPUT_TOUCHSCREEN=1")
    if is_joystick:
        result.append("ID_INPUT_JOYSTICK=1")
    if is_tablet:
        result.append("ID_INPUT_TABLET=1")
    if is_tablet_pad:
        result.append("ID_INPUT_TABLET_PAD=1")
    return result, any(
        (
            is_tablet,
            is_mouse,
            is_abs_mouse,
            is_touchpad,
            is_touchscreen,
            is_joystick,
            is_pointing_stick,
        )
    )


def key_properties(ev: int, key: int) -> tuple[list[str], bool]:
    if not bit(ev, EV_KEY):
        return [], False
    found = any_bit(key, 0, BTN_MISC)
    if not found:
        found = any_bit(key, KEY_OK, BTN_DPAD_UP) or any_bit(
            key, KEY_ALS_TOGGLE, BTN_TRIGGER_HAPPY
        )
    result = ["ID_INPUT_KEY=1"] if found else []
    if key & 0xFFFFFFFE == 0xFFFFFFFE:
        result.append("ID_INPUT_KEYBOARD=1")
        return result, True
    return result, found


def derive(device: Path) -> list[str]:
    capabilities = device / "capabilities"
    ev = read_bitmap(capabilities / "ev")
    absolute = read_bitmap(capabilities / "abs")
    key = read_bitmap(capabilities / "key")
    relative = read_bitmap(capabilities / "rel")
    properties = read_bitmap(device / "properties")
    bustype = int((device / "id" / "bustype").read_text(encoding="ascii"), 16)

    result = ["ID_INPUT=1"]
    pointer, is_pointer = pointer_properties(
        bustype, ev, absolute, key, relative, properties
    )
    keys, is_key = key_properties(ev, key)
    result.extend(pointer)
    result.extend(keys)
    if (
        not is_pointer
        and not is_key
        and bit(ev, EV_REL)
        and (bit(relative, REL_WHEEL) or bit(relative, REL_HWHEEL))
    ):
        result.append("ID_INPUT_KEY=1")
    if bit(ev, EV_SW):
        result.append("ID_INPUT_SWITCH=1")
    return result


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("device", type=Path, help="inputN sysfs directory")
    args = parser.parse_args()
    for item in derive(args.device):
        print(item)


if __name__ == "__main__":
    main()
