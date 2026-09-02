#!/usr/bin/env python3
"""Run ``apk add fastfetch`` in an Xfce terminal through QMP keyboard input.

The Xfce session owns the virtio console, so the normal shell-oriented QEMU
tests cannot issue a command after the desktop has started.  This probe uses
the same Ctrl+Alt+T path as a user and leaves the result in the persistent apk
database for the host-side test to inspect.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import socket
import time


class QMP:
    def __init__(self, path: str) -> None:
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(10.0)
        self.sock.connect(path)
        self.stream = self.sock.makefile("rwb", buffering=0)
        greeting = self._read()
        if "QMP" not in greeting:
            raise RuntimeError(f"invalid QMP greeting: {greeting!r}")
        self.execute("qmp_capabilities")

    def _read(self) -> dict:
        while True:
            line = self.stream.readline()
            if not line:
                raise EOFError("QMP socket closed")
            message = json.loads(line)
            if "event" not in message:
                return message

    def execute(self, name: str, arguments: dict | None = None) -> object:
        request: dict[str, object] = {"execute": name}
        if arguments is not None:
            request["arguments"] = arguments
        self.stream.write(json.dumps(request).encode() + b"\r\n")
        response = self._read()
        if "error" in response:
            raise RuntimeError(f"QMP {name} failed: {response['error']}")
        return response.get("return")

    @staticmethod
    def key_event(code: str, down: bool) -> dict:
        return {
            "type": "key",
            "data": {
                "down": down,
                "key": {"type": "qcode", "data": code},
            },
        }

    def chord(self, *codes: str) -> None:
        events = [self.key_event(code, True) for code in codes]
        events.extend(self.key_event(code, False) for code in reversed(codes))
        self.execute("input-send-event", {"events": events})

    def key(self, code: str) -> None:
        self.execute(
            "input-send-event",
            {"events": [self.key_event(code, True), self.key_event(code, False)]},
        )

    def type_ascii(self, text: str) -> None:
        for character in text:
            code = "spc" if character == " " else character
            if not (code == "spc" or "a" <= code <= "z"):
                raise ValueError(f"unsupported probe character: {character!r}")
            self.key(code)
            time.sleep(0.025)

    def close(self) -> None:
        self.stream.close()
        self.sock.close()


def main() -> int:
    qmp = QMP(os.environ["XFCE_APK_QMP_SOCKET"])
    try:
        # Xorg-ready precedes xfwm4, the panel, and shortcut registration.
        time.sleep(float(os.environ.get("XFCE_APK_DESKTOP_WAIT", "65")))
        launcher = os.environ.get("XFCE_APK_LAUNCHER", "terminal")
        if launcher == "terminal":
            qmp.chord("ctrl", "alt", "t")
        elif launcher == "appfinder":
            qmp.chord("alt", "f2")
        else:
            raise ValueError(f"invalid XFCE_APK_LAUNCHER={launcher!r}")
        time.sleep(float(os.environ.get("XFCE_APK_TERMINAL_WAIT", "25")))
        qmp.type_ascii("apk add fastfetch")
        qmp.key("ret")
        time.sleep(float(os.environ.get("XFCE_APK_INSTALL_WAIT", "45")))
        screenshot = os.environ.get("XFCE_APK_SCREENSHOT")
        if screenshot:
            path = Path(screenshot).resolve()
            qmp.execute(
                "screendump",
                {"filename": str(path), "format": "ppm", "device": "pachagpu"},
            )
        print("XFCE_APK_QMP_DONE", flush=True)
        return 0
    finally:
        qmp.close()


if __name__ == "__main__":
    raise SystemExit(main())
