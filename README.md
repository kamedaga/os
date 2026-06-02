# PachaOS

> Capability-based pure microkernel OS written in Zig — Linux ABI compatible, userland-first.

![Zig](https://img.shields.io/badge/kernel-Zig-f7a41d?style=flat-square&logo=zig)
![License: MIT](https://img.shields.io/badge/license-MIT-blue?style=flat-square)
![Platform](https://img.shields.io/badge/platform-x86__64-lightgrey?style=flat-square)
![Status](https://img.shields.io/badge/status-experimental-orange?style=flat-square)
![Linux ABI](https://img.shields.io/badge/Linux%20ABI-compatible-brightgreen?style=flat-square&logo=linux)

既存の Linux エコシステムとの互換を目指すマイクロカーネル。

---

## Features

- **Pure Microkernel** — カーネルは capability 管理・スケジューリング・trap delegation のみを担当
- **Trap Delegation** — Linux syscall をカーネルが解釈せず、capability で制御されたユーザーランドサーバーが処理
- **Hardware Capabilities** — capsule を用いた、DMA バッファ・IOMMU マッピング・Virtqueue の capability 抽象化
- **Linux ABI Compatibility** — 無改造の musl libc を動的リンクでロードし、Linux syscall をユーザーランドで処理
- **Userland Drivers** — virtio-blk / virtio-net などドライバはすべてユーザー空間で動作
- **x86_64** — x86_64 対応。AArch64 は今後対応予定

## Tech Stack

| Layer | Language | Detail |
|---|---|---|
| Kernel | Zig | Freestanding / UEFI boot / x86_64 |
| Userland | C / CMake | musl libc / ELF loader / Linux ABI server |

---

## Linux Applications

Trap delegation により、無改造の Linux バイナリがそのまま動作します。  
musl ビルドで確認済み（glibc も動作）。

`Python 3` &nbsp; `Vim` &nbsp; `Clang` &nbsp; `apk` &nbsp; `nano` &nbsp; `Lua` &nbsp; `W3M`

### Python3 on PachaOS

```pycon
Python 3.12.13 (main, Apr 10 2026, 14:16:05) [GCC 14.2.0] on linux
Type "help", "copyright", "credits" or "license" for more information.
>>> import os
>>> print("os.uname():", os.uname())
os.uname(): posix.uname_result(sysname='Linux', nodename='capabilityos', release='6.0.0-capabilityos', version='CapabilityOS Linux ABI', machine='x86_64')
>>>
```

### apk + clang on PachaOS

```sh
# apk add nano
fetch http://dl-cdn.alpinelinux.org/alpine/v3.22/main/x86_64/APKINDEX.tar.gz
fetch http://dl-cdn.alpinelinux.org/alpine/v3.22/community/x86_64/APKINDEX.tar.gz
(1/3) Installing ncurses-terminfo-base (6.5_p20250503-r0)
(2/3) Installing libncursesw (6.5_p20250503-r0)
(3/3) Installing nano (8.4-r0)
OK: 464 MiB in 28 packages
# nano main.c
```

![nano editing main.c](assets/nano-screenshot.png)

```sh
# clang main.c
# ls
a.out  main.c
# ./a.out
hello world
```

---

## kobox — Linux Kernel Driver Runtime

[![kobox](https://img.shields.io/badge/kobox-GitHub-black?style=flat-square&logo=github)](https://github.com/kamedaga/kobox)

[kobox](https://github.com/kamedaga/kobox) は Linux カーネル向けドライバ (`.ko`) をユーザーランドプロセスとして直接実行するランタイムです。  
バックエンドを実装することで任意の OS に対応でき、PachaOS では独自 Capsule を用いて動作します。

**PachaOS Capsule バックエンドで動作中:**

| Driver | Module |
|---|---|
| NVMe | `nvme.ko` / `nvme-core.ko` |
| USB Storage | `usbcore.ko` / `usb-storage.ko` / `xhci-hcd.ko` |

---

## Build

```powershell
pactl setup full
pactl run
```

詳細は [ビルドガイド](pacha_docs/build.md) を参照してください（動作確認済みバージョン・依存関係など）。

---

## License

PachaOS source code is licensed under the **MIT License**.  
Third-party runtime components are documented in [THIRD_PARTY_NOTICES.md](./THIRD_PARTY_NOTICES.md).
