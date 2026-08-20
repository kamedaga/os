# PachaOS

> FD pure microkernel OS written in Zig — Everything is a File Descriptor.

![Zig](https://img.shields.io/badge/kernel-Zig-f7a41d?style=flat-square&logo=zig)
![Go](https://img.shields.io/badge/tooling-Go-00ADD8?style=flat-square&logo=go)
![Nix](https://img.shields.io/badge/env-Nix-5277C3?style=flat-square&logo=nixos)
![License: MIT](https://img.shields.io/badge/license-MIT-blue?style=flat-square)
![Platform](https://img.shields.io/badge/platform-x86__64-lightgrey?style=flat-square)
![Status](https://img.shields.io/badge/status-experimental-orange?style=flat-square)
![Linux ABI](https://img.shields.io/badge/Linux%20ABI-compatible-brightgreen?style=flat-square&logo=linux)

既存の Linux エコシステムとの互換を目指す FD capability ベースのマイクロカーネルです。

Linux application も特別な仮想マシンや Linux process ではなく、必要最小限の
FD capability を持つ普通の PachaOS process として動きます。Linux syscall は
kernel や特権 server ではなく、process に動的リンクされた非特権の LPR が
userland で実装します。詳しくは [PachaOS の設計思想](pacha_docs/architecture.md)
を参照してください。

---

## Features

- **FD-based Microkernel** — 全ての process が、rights を縮小して受け渡せる FD capability で authority を持ちます
- **Linux Personality Runtime** — zpoline で Linux syscall を process 内の関数呼び出しへ変換する、非特権の動的リンク runtime です
- **Kobox** — capsule を用いた、ユーザー空間で動くLinuxカーネルモジュールの変換レイヤー
- **x86_64** — x86_64 対応。AArch64 は今後対応予定
- **Native Libc** — musl libcを互換レイヤーを用いず、ネイティブで動かせます

## Update
- Linux Personality Runtimeを実装しました
- カーネルをFD-based Microkernelへ変更しました
- 独自ドライバからkoboxに全面移行しました。
- Linux ABIレイヤーなしで musl libcに対応しました。

## Tech Stack

## Language
| Layer | Language | Detail |
|---|---|---|
| Kernel | Zig / C / Rocq | Freestanding / UEFI boot / x86_64 |
| Userland | C / CMake | musl libc / ELF loader / per-process Linux Personality Runtime |
| Tools | Go / Nix / bash | Build Tools  |

## Core Functions

| Name | Detail |
|---|---|
| Filed | Ext4 / vnode / NVMe|
| Netd | libuinet / net-driver |
| Scheduler | EEVDF / SMP |
| seed | Init |
| Termd | Linux TTY |
| Drmd | DRM / KMS |
| LPR | Zpoline / LinuxShim ...|



---

## Linux Applications

Trap delegation からLinux Personality Runtimeに切り替え、高速でシンプルに互換レイヤーが処理できるようになりました。以前動いたバイナリも、段階的に動くようにします。
musl ビルドで確認済み。

`apk` &nbsp; `Lua` &nbsp; `Chibicc` &nbsp; `busybox` &nbsp; `GNU Coreutils` &nbsp; `Python3` &nbsp; `Clang` &nbsp; `Mesa`  &nbsp; `Sway`

### Python3 on PachaOS

```pycon
Python 3.12.13 (main, Apr 10 2026, 14:16:05) [GCC 14.2.0] on linux
Type "help", "copyright", "credits" or "license" for more information.
>>> import os
>>> print("os.uname():", os.uname())
os.uname(): posix.uname_result(sysname='Linux', nodename='capabilityos', release='6.0.0-capabilityos', version='CapabilityOS Linux ABI', machine='x86_64')
>>>
```

**Kobox**

| Module | Module |
|---|---|
| NVMe | `nvme.ko` / `nvme-core.ko` |
| USB Storage | `usbcore.ko` / `usb-storage.ko` / `xhci-hcd.ko` |
| USB HID(マウスで実験中) | `usbcore.ko` / `hid.ko` / `hid-generic.ko` / `usbhid.ko` / `xhci-hcd.ko`|
| Ext4 | `crc16.ko` / `mbcache.ko` / `jbd2.ko` / `ext4.ko`|
| virtio-net | `virtio.ko` / `virtio_ring` / `virtio_pci.ko` / `failover.ko` / `net_failover.ko `/ `virtio_net.ko` |
| linux tty | `linux_tty_core.ko` |
| virtio-input | `linux_virtio_input.ko` ...|


Kobox は FD capability を使う userland component です。storage 用 runtime は
VFS/execとの責務境界を保ったまま、現在は `filed.elf` にリンクされています。
TTY、network、display、input など、独立した状態と回復単位を持つ subsystem は
それぞれ別の service process に配置されます。

※koboxはApache 2.0でライセンスされてます。

---

## Build

```powershell
nix develop
./pacgo sync rootfs
./pacgo qemu --new-terminal
```

詳細は [ビルドガイド](pacha_docs/build.md) を参照してください（動作確認済みバージョン・依存関係など）。

---

## License

PachaOS source code is licensed under the **MIT License**.  
Third-party runtime components are documented in [THIRD_PARTY_NOTICES.md](./THIRD_PARTY_NOTICES.md).
