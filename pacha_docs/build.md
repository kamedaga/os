# Build and Boot

WSL Linux 上で動作確認を行っている。

## 構成

| Path | Role |
|---|---|
| `pacgo` | Nix devShell 内で動く Go 製 runner |
| `pack/` | pacgo 本体・CLI・pack 定義 |
| `pack/pack.yaml` | kernel / apps / rootfs / startup / disk の宣言的定義 |
| `nix/` / `flake.nix` | WSL Linux 用 toolchain 環境 |
| `.artifacts/` | 生成物・disk image・manifest・pacgo state |

## Requirements

```bash
nix develop
```

これで toolchain が全部入る。以降のコマンドはこの shell の中で叩く。

## Plan

今の workspace と app 状態を確認する。

```bash
./pacgo plan
```

## Build

userland をビルドして rootfs に同期:

```bash
./pacgo build userland
```

rootfs 同期はせず artifact だけ確認したい場合:

```bash
./pacgo build userland --no-rootfs
```

特定 app だけ:

```bash
./pacgo build userland exec
./pacgo build userland fastfetch
```

kernel をビルド:

```bash
./pacgo build kernel
```

出力: `kernel/zig-out/bin/EFI/BOOT/BOOTX64.EFI`

## Manifests

```bash
./pacgo gen manifests
```

| Manifest | Path |
|---|---|
| bootfs | `.artifacts/manifests/bootfs.generated.txt` |
| rootfs | `.artifacts/manifests/rootfs.generated.txt` |
| startup | `.artifacts/manifests/startup.generated.txt` |

## Sync

rootfs を差分同期:

```bash
./pacgo sync rootfs
```

bootfs / ESP を更新:

```bash
./pacgo sync bootfs
```

## QEMU

起動だけ:

```bash
./pacgo qemu
```

build / sync も一緒にやってから起動:

```bash
./pacgo qemu --prepare
```

virtio-console を別 terminal に分ける:

```bash
./pacgo qemu --new-terminal
```

ログ: `.artifacts/qemu.log`

## Tests

boot smoke:

```bash
./pacgo test smoke
```

TTY interaction test:

```bash
./pacgo qemu-test
```

任意の文字列を送る場合:

```bash
./pacgo qemu-test --send "/bin/fastfetch" --expect "PachaOS"
```

## Progress

CI やログ保存したい場合は plain 表示:

```bash
PACGO_PROGRESS=plain ./pacgo sync rootfs
```

## Generated Files

`.artifacts/` 以下に置く。Git には含めない。

| Path | Role |
|---|---|
| `.artifacts/bin/pacgo` | compiled pacgo binary |
| `.artifacts/disk.img` | QEMU boot disk |
| `.artifacts/manifests/` | generated manifests |
| `.artifacts/userland/` | normalized userland artifacts |
