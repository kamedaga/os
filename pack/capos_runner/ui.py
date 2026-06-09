from __future__ import annotations

import sys
import time
import shlex
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

try:
    from rich.console import Console
    from rich.panel import Panel
    from rich.table import Table
    from rich.theme import Theme

    RICH_AVAILABLE = True
except ModuleNotFoundError:  # pragma: no cover - exercised only outside Nix env
    Console = None  # type: ignore[assignment]
    Panel = None  # type: ignore[assignment]
    Table = None  # type: ignore[assignment]
    Theme = None  # type: ignore[assignment]
    RICH_AVAILABLE = False


@dataclass
class Timer:
    started: float

    @classmethod
    def start(cls) -> "Timer":
        return cls(time.monotonic())

    def elapsed(self) -> str:
        seconds = time.monotonic() - self.started
        if seconds < 1:
            return f"{seconds * 1000:.0f}ms"
        return f"{seconds:.1f}s"


class PlainConsole:
    def print(self, *objects: object, sep: str = " ", end: str = "\n", **_: object) -> None:
        text = sep.join(str(item) for item in objects)
        sys.stdout.write(text + end)


if RICH_AVAILABLE:
    theme = Theme(
        {
            "task": "bold cyan",
            "muted": "dim",
            "ok": "bold green",
            "warn": "bold yellow",
            "err": "bold red",
            "key": "cyan",
            "value": "white",
        }
    )
    console = Console(theme=theme)
    error_console = Console(stderr=True, theme=theme)
else:  # pragma: no cover
    console = PlainConsole()
    error_console = console


def task(name: str, detail: str | None = None) -> None:
    suffix = f" [muted]{detail}[/muted]" if detail else ""
    console.print(f"\n[task]> Task :{name}[/task]{suffix}")


def success(label: str, elapsed: str) -> None:
    console.print(f"\n[ok]BUILD SUCCESSFUL[/ok] in {elapsed}  [muted]{label}[/muted]")


def failure(label: str, elapsed: str, message: str) -> None:
    console.print(f"\n[err]BUILD FAILED[/err] in {elapsed}  [muted]{label}[/muted]")
    error_console.print(f"[err]{message}[/err]")


def key_values(title: str, rows: Iterable[tuple[str, object]]) -> None:
    rows = list(rows)
    if RICH_AVAILABLE:
        table = Table(title=title, show_header=False, box=None, padding=(0, 1))
        table.add_column("key", style="key", no_wrap=True)
        table.add_column("value", style="value")
        for key, value in rows:
            table.add_row(key, str(value))
        console.print(table)
        return

    console.print(title)
    for key, value in rows:
        console.print(f"  {key}: {value}")


def table(title: str, columns: list[str], rows: Iterable[Iterable[object]]) -> None:
    rows = [list(row) for row in rows]
    if RICH_AVAILABLE:
        rich_table = Table(title=title, expand=False)
        for index, column in enumerate(columns):
            style = "key" if index == 0 else "value"
            rich_table.add_column(column, style=style)
        for row in rows:
            rich_table.add_row(*(str(value) for value in row))
        console.print(rich_table)
        return

    console.print(title)
    console.print("\t".join(columns))
    for row in rows:
        console.print("\t".join(str(value) for value in row))


def paths(title: str, entries: Iterable[tuple[str, str]], limit: int) -> None:
    entries = list(entries)
    shown = entries[:limit]
    table(title, ["State", "Path"], shown)
    remaining = len(entries) - len(shown)
    if remaining > 0:
        console.print(f"[muted]... {remaining} more path(s)[/muted]")


def command(label: str, argv: list[str], cwd: Path) -> None:
    rendered = " ".join(shlex.quote(part) for part in argv)
    console.print(f"[muted]{label}: ({cwd}) {rendered}[/muted]")
