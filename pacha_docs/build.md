# Build and Boot

PachaOS / CapabilityOS の現在のビルド手順です。ビルドは WSL Linux 上で完結させます。Windows 側の Zig、Rust、Python、pactl は使いません。

## Overview

現在の主な構成は次です。

| Path | Role |
|---|---|
| `pacgo` | Nix devShell に入り、Go 製 runner を起動する薄い wrapper |
| `pack/` | pacgo 本体、CLI、pack 定義 |
| `pack/pack.yaml` | kernel、apps、rootfs、startup、disk の宣言的定義 |
| `nix/` / `flake.nix` | WSL Linux 用 toolchain 環境 |
| `.artifacts/` | 生成物、disk image、manifest、pacgo state |
| `tools/` | ローカル helper 置き場。GitHub には追加しない |

`tools/` と `flake.lock` は `.gitignore` 対象です。`tools/` は現在のローカル作業用 helper を含みますが、公開 GitHub に追加しません。今後は必要なものを `pack/` や Nix 側へ整理して移します。

## Requirements

WSL Linux 上で次を使います。

| Tool | Source |
|---|---|
| Nix | host にインストール |
| Go | Nix devShell または host |
| Zig | Nix devShell |
| Clang / GCC / CMake | Nix devShell |
| QEMU / OVMF | Nix devShell |
| mtools / dosfstools / e2fsprogs | Nix devShell |

通常は `./pacgo ...` を実行すれば、wrapper が自動で `nix develop` に入ります。

```bash
./pacgo plan
```

明示的に shell に入りたい場合:

```bash
nix develop
```

`flake.lock` はローカル lock として扱います。GitHub には追加しません。完全に同一の nixpkgs revision を固定したい場合は、各自の作業環境で生成された `flake.lock` をローカルに保持してください。

## Plan

現在の workspace と app 状態を確認します。

```bash
./pacgo plan
```

表示される主な項目:

| Field | Meaning |
|---|---|
| `definition` | `pack/pack.yaml` |
| `apps` | active / skipped app 数 |
| `disk` | `.artifacts/disk.img` |
| `manifests` | `.artifacts/manifests` |
| `skip apps` | `pack.yaml` で無効化されている app |

## Build Userland

userland apps をビルドします。通常は rootfs 同期まで行います。

```bash
./pacgo build userland
```

rootfs へ同期せず artifact だけ確認する場合:

```bash
./pacgo build userland --no-rootfs
```

特定 app のみ:

```bash
./pacgo build userland exec
./pacgo build userland fastfetch
```

pacgo は content fingerprint を使って source の変更を検出します。`/mnt/c` の clock skew に依存しないため、mtime だけの判定より安定しています。

`--no-rootfs` で artifact だけ更新した場合も、未同期 artifact は `.artifacts/pack/dirty-artifacts.txt` に記録されます。次の `./pacgo sync rootfs` で rootfs へ反映されます。

## Build Kernel

kernel EFI artifact をビルドします。

```bash
./pacgo build kernel
```

出力:

```text
kernel/zig-out/bin/EFI/BOOT/BOOTX64.EFI
```

kernel を直接確認する場合は `kernel/` を作業ディレクトリにします。

```bash
cd kernel
zig build efi
```

repo root で `zig build-obj` を直接叩かないでください。root 直下に object file を撒きやすいためです。

## Manifests

manifest を生成します。

```bash
./pacgo gen manifests
```

出力:

| Manifest | Path |
|---|---|
| bootfs | `.artifacts/manifests/bootfs.generated.txt` |
| rootfs | `.artifacts/manifests/rootfs.generated.txt` |
| startup | `.artifacts/manifests/startup.generated.txt` |

rootfs manifest は directory artifact も展開します。`alpine_clang` や `alpine_go` のような大きい tree が active な場合、entry 数が増えます。

## Sync Rootfs

rootfs partition へ差分同期します。

```bash
./pacgo sync rootfs
```

流れ:

1. `build:userland`
2. `init:disk`
3. `gen:manifests`
4. `sync:rootfs`

rootfs は FAT32 partition です。通常は content fingerprint と layout cache により up-to-date 判定または targeted incremental sync になります。

期待される no-op の例:

```text
Rootfs
  state          up-to-date
  updated files  0
```

source を編集して artifact が変わった場合:

```text
Rootfs
  state          synced
  updated files  1
```

既存 FAT layout cache が古い場合、pacgo は incremental を諦めて full rewrite に fallback します。

## Sync Bootfs / ESP

BOOTFS.IMG と EFI System Partition を更新します。

```bash
./pacgo sync bootfs
```

これは kernel boot に必要な EFI 側 artifact を同期します。

## QEMU

QEMU は起動だけを行います。build や sync は自動では行いません。

```bash
./pacgo qemu
```

起動前に build / sync も行いたい場合:

```bash
./pacgo qemu --prepare
```

virtio-console を別 terminal に分けたい場合:

```bash
./pacgo qemu --new-terminal
```

QEMU log:

```text
.artifacts/qemu.log
```

## Tests

boot smoke test:

```bash
./pacgo test
./pacgo test smoke
```

QEMU TTY interaction test:

```bash
./pacgo qemu-test
```

default は boot marker を待ってから `/bin/fastfetch` を送信し、console output に `PachaOS` が出ることを確認します。

任意の文字列を送る場合:

```bash
./pacgo qemu-test --send "/bin/fastfetch" --expect "PachaOS"
```

Python で細かく確認する場合:

```bash
./pacgo qemu-test --python pack/examples/qemu_tty_test.py
```

Python script には次の環境変数が渡されます。

| Env | Meaning |
|---|---|
| `PACGO_QEMU_CONSOLE` | virtio-console Unix socket |
| `PACGO_QEMU_SERIAL_LOG` | serial log path |
| `PACGO_QEMU_CONSOLE_LOG` | console log path |
| `PACGO_QEMU_LOG` | QEMU debug log path |
| `PACGO_QEMU_BOOT_MARKER` | boot marker |
| `PACGO_QEMU_TIMEOUT_SECONDS` | timeout |

## Progress Output

pacgo は長い処理中に loading log と progress bar を出します。

TTY では一時的な1行表示になります。ログに残したい場合や CI では plain 表示を使えます。

```bash
PACGO_PROGRESS=plain ./pacgo sync rootfs
```

強制的に progress bar を出す場合:

```bash
PACGO_PROGRESS=always ./pacgo gen manifests
```

## Runtime Inputs

runtime input は `pack/pack.yaml` の apps で管理します。代表例:

| App | Role |
|---|---|
| `apk` | Alpine apk-tools |
| `fastfetch` | TTY smoke / environment display |
| `alpine_go` | Alpine Go toolchain root |
| `alpine_clang` | Alpine Clang/GCC toolchain root |
| `exec_service` | process builder service |
| `fat_server` | FAT rootfs service |
| `linux_abi_server` | Linux ABI userland service |
| `tty_service` | console / TTY service |

`skip.apps` で一時的に app を無効化できます。

```yaml
skip:
  apps: ["persistent_fs", "seed", "shell"]
```

## Generated Files

生成物は `.artifacts/` に置きます。Git には含めません。

| Path | Role |
|---|---|
| `.artifacts/bin/pacgo` | compiled pacgo binary |
| `.artifacts/disk.img` | QEMU boot disk |
| `.artifacts/manifests/` | generated manifests |
| `.artifacts/pack/` | pacgo state, fingerprints, dirty artifact queue |
| `.artifacts/userland/` | normalized userland artifacts |
| `.artifacts/userland-fixtures/` | generated external/runtime fixtures |
| `.artifacts/cmake/` | CMake build directories |

## Notes

- pactl は現在の build path では使いません。
- Windows 側 toolchain は使いません。
- `tools/` は GitHub に追加しません。必要な helper は今後 `pack/` または Nix 側へ移します。
- rootfs は現在 FAT32 です。将来的に ext4 も視野に入れます。
