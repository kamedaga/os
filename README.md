# PachaOS

Capability-based pure microkernel OS written in Zig.

musl libc と動的リンクにより、既存の Linux エコシステムとの互換を目指す。

## Features

- **Pure microkernel** — カーネルは capability 管理・スケジューリング・trap delegation のみを担当
- **Trap delegation** — syscall をカーネルが解釈せず、capability で制御されたユーザーランドサーバーに委譲
- **Hardware capabilities** — DMA バッファ・IOMMU マッピング・Virtqueue を capability として抽象化
- **Linux ABI compatibility** — 無改造の musl libc を動的リンクでロードし、ユーザーランドで Linux syscall を処理
- **Userland drivers** — virtio-blk / virtio-gpu ドライバはすべてユーザー空間で動作
- **x86_64 support** — x86_64のみに対応 AArch64に対応予定
## Design

カーネルは Linux syscall の意味を持たない。fd table、errno、パス解決、filesystem semantics はすべてユーザーランドの `linux_abi_server` に閉じる。

カーネルの設計目標は 20,000 行以下。Lean 4 形式検証目標。

## Tech Stack

| Layer | Language | Note |
|---|---|---|
| Kernel | Zig | Freestanding, UEFI boot, x86_64 |
| Userland | C / CMake | musl libc, ELF loader, ABI server |

## Build

```bash
pactl setup full
pactl run
```

## Status

Kernel は安定動作。ユーザー空間 ELF ローダー・動的リンカが動作し、musl libc のロードとエントリ到達を確認。Trap delegation と linux_abi_server を実装中。

## License

MIT