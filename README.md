# PachaOS

Capability-based pure microkernel OS written in Zig.

musl libc と動的リンクにより、既存の Linux エコシステムとの互換を目指す。

## Features

- **Pure microkernel** — カーネルは capability 管理・スケジューリング・trap delegation のみを担当
- **Trap delegation** — syscall をカーネルが解釈せず、capability で制御されたユーザーランドサーバーに委譲
- **Hardware capabilities** — DMA バッファ・IOMMU マッピング・Virtqueue を capability として抽象化
- **Linux ABI compatibility** — 無改造の musl libc を動的リンクでロードし、ユーザーランドで Linux syscall を処理
- **Userland drivers** — virtio-blk / virtio-net などドライバはすべてユーザー空間で動作
- **x86_64 support** — x86_64のみに対応 いずれAArch64に対応予定
## Design

カーネルは Linux syscall の意味を持たない。

カーネルの設計目標は 20,000 行以下。Lean 4 形式検証目標。

## python3 OS上で動作 
```pycon
Python 3.12.13 (main, Apr 10 2026, 14:16:05) [GCC 14.2.0] on linux
Type "help", "copyright", "credits" or "license" for more information.
>>> import os
>>> print("os.uname():", os.uname())
os.uname(): posix.uname_result(sysname='Linux', nodename='capabilityos', release='6.0.0-capabilityos', version='CapabilityOS Linux ABI', machine='x86_64')
>>>
```

## OS上でclangを用いたコンパイル
```dash
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
```dash
# clang main.c
# ls
a.out  main.c
# ./a.out
hello world
```


## Tech Stack

| Layer | Language | Note |
|---|---|---|
| Kernel | Zig | Freestanding, UEFI boot, x86_64 |
| Userland | C / CMake | musl libc, ELF loader, ABI server |

## Build

```powershell
pactl setup full
pactl run
```
・[ビルド方法](pacha_docs/build.md) : 細かいビルドの方法や、動作確認されたバージョンなど

## Status

Kernel は安定動作。ユーザー空間 ELF ローダー・動的リンカがマルチコアで動作し、apkからパッケージを追加して動いています。(nano, w3m, cpython, lua, vimなど)

また、muslだけでなく、musl変換レイヤーを介さずglibcの実行ファイルでhello, worldも成功してる。


## License

CapabilityOS source code is licensed under the MIT License. Included or
generated third-party runtime components are documented in
[THIRD_PARTY_NOTICES.md](./THIRD_PARTY_NOTICES.md).
